//
// driver for qemu's virtio disk device.
// uses qemu's mmio interface to virtio.
//
// qemu ... -drive file=fs.img,if=none,format=raw,id=x0 -device
// virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "virtio.h"

// the address of virtio mmio register r.
// 寄存器访问宏
#define R(r) ((volatile uint32*)(VIRTIO0 + (r)))

// 磁盘设备结构
static struct disk
{
    // a set (not a ring) of DMA descriptors, with which the
    // driver tells the device where to read and write individual
    // disk operations. there are NUM descriptors.
    // most commands consist of a "chain" (a linked list) of a couple of
    // these descriptors.
    struct virtq_desc* desc;   // 描述符表

    // a ring in which the driver writes descriptor numbers
    // that the driver would like the device to process.  it only
    // includes the head descriptor of each chain. the ring has
    // NUM elements.
    struct virtq_avail* avail;   // 可用环

    // a ring in which the device writes descriptor numbers that
    // the device has finished processing (just the head of each chain).
    // there are NUM used ring entries.
    struct virtq_used* used;   // 已使用环

    // our own book-keeping.
    char   free[NUM];   // 描述符是否空闲
    uint16 used_idx;    // 已使用环的处理索引

    // track info about in-flight operations,
    // for use when completion interrupt arrives.
    // indexed by first descriptor index of chain.
    struct
    {
        struct buf* b;        // 关联的缓冲区
        char        status;   // 请求状态
    } info[NUM];              // 跟踪正在进行的操作

    // disk command headers.
    // one-for-one with descriptors, for convenience.
    struct virtio_blk_req ops[NUM];   // 磁盘请求头部

    struct spinlock vdisk_lock;   // 同步锁

} disk;

// 初始化 VirtIO 块设备，设置设备状态、协商功能、配置队列，并标记设备为就绪。
void virtio_disk_init(void)
{
    uint32 status = 0;
    // 初始化磁盘操作的自旋锁
    initlock(&disk.vdisk_lock, "virtio_disk");
    // 检查设备标识寄存器
    if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 || *R(VIRTIO_MMIO_VERSION) != 2 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != 2 || *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)
    {
        panic("could not find virtio disk");
    }

    // 写 0 重置设备e
    *R(VIRTIO_MMIO_STATUS) = status;

    // 添加 ACKNOWLEDGE 状态位，通知设备已被识别
    status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 添加 DRIVER 状态位，通知驱动已加载
    status |= VIRTIO_CONFIG_S_DRIVER;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 读取设备支持的特性
    uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);

    // 禁用不需要的特性位
    features &= ~(1 << VIRTIO_BLK_F_RO);
    features &= ~(1 << VIRTIO_BLK_F_SCSI);
    features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
    features &= ~(1 << VIRTIO_BLK_F_MQ);
    features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
    features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);

    // 回写协商后的特性
    *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

    // 标记特性协商完成
    status |= VIRTIO_CONFIG_S_FEATURES_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // 验证设备是否接受特性
    status = *R(VIRTIO_MMIO_STATUS);
    if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk FEATURES_OK unset");

    // 选择队列 0
    *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

    // 确保队列未被激活
    if (*R(VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk should not be ready");

    // 读取队列最大容量
    uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (max == 0)
        panic("virtio disk has no queue 0");
    if (max < NUM)
        panic("virtio disk max queue too short");

    // 分配VirtIO队列内存
    disk.desc  = (struct virtq_desc*)kalloc();
    disk.avail = (struct virtq_avail*)kalloc();
    disk.used  = (struct virtq_used*)kalloc();
    if (!disk.desc || !disk.avail || !disk.used)
        panic("virtio disk kalloc");
    memset(disk.desc, 0, PGSIZE);
    memset(disk.avail, 0, PGSIZE);
    memset(disk.used, 0, PGSIZE);

    // 设置队列大小
    *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

    // 写入描述符表物理地址（64 位拆分为低/高 32 位）
    *R(VIRTIO_MMIO_QUEUE_DESC_LOW)  = (uint64)disk.desc;
    *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;

    // 写入可用环地址（驱动->设备）
    *R(VIRTIO_MMIO_DRIVER_DESC_LOW)  = (uint64)disk.avail;
    *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;

    // 写入已用环地址（设备->驱动）
    *R(VIRTIO_MMIO_DEVICE_DESC_LOW)  = (uint64)disk.used;
    *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

    // 激活队列
    *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

    // 标记所有描述符为空闲可用
    for (int i = 0; i < NUM; i++)
        disk.free[i] = 1;

    // 设置 DRIVER_OK 状态位，通知设备驱动完全就绪
    status |= VIRTIO_CONFIG_S_DRIVER_OK;
    *R(VIRTIO_MMIO_STATUS) = status;

    // plic.c and trap.c arrange for interrupts from VIRTIO0_IRQ.
}

// find a free descriptor, mark it non-free, return its index.
// 在 disk.free 数组中寻找一个空闲描述符，标记为非空闲并返回其索引
static int alloc_desc()
{
    for (int i = 0; i < NUM; i++)
    {
        if (disk.free[i])
        {
            disk.free[i] = 0;
            return i;
        }
    }
    return -1;
}

// mark a descriptor as free.
// 释放指定索引 i 的描述符，标记为空闲并唤醒等待空闲描述符的进程
static void free_desc(int i)
{
    if (i >= NUM)
        panic("free_desc 1");
    if (disk.free[i])
        panic("free_desc 2");
    disk.desc[i].addr  = 0;
    disk.desc[i].len   = 0;
    disk.desc[i].flags = 0;
    disk.desc[i].next  = 0;
    disk.free[i]       = 1;
    wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void free_chain(int i)
{
    while (1)
    {
        int flag = disk.desc[i].flags;
        int nxt  = disk.desc[i].next;
        free_desc(i);
        if (flag & VRING_DESC_F_NEXT)
            i = nxt;
        else
            break;
    }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int alloc3_desc(int* idx)
{
    for (int i = 0; i < 3; i++)
    {
        idx[i] = alloc_desc();
        if (idx[i] < 0)
        {
            for (int j = 0; j < i; j++)
                free_desc(idx[j]);
            return -1;
        }
    }
    return 0;
}

void virtio_disk_rw(struct buf* b, int write)
{
    // 计算磁盘扇区地址
    uint64 sector = b->blockno * (BSIZE / 512);
    // 获取磁盘操作锁
    acquire(&disk.vdisk_lock);

    // the spec's Section 5.2 says that legacy block operations use
    // three descriptors: one for type/reserved/sector, one for the
    // data, one for a 1-byte status result.

    // 分配描述符链
    int idx[3];
    while (1)
    {
        if (alloc3_desc(idx) == 0)
        {
            break;
        }
        sleep(&disk.free[0], &disk.vdisk_lock);
    }

    // format the three descriptors.
    // qemu's virtio-blk.c reads them.

    // 初始化请求头
    // type：操作类型（读/写）
    // sector：目标扇区地址
    struct virtio_blk_req* buf0 = &disk.ops[idx[0]];
    if (write)
        buf0->type = VIRTIO_BLK_T_OUT;   // 写磁盘
    else
        buf0->type = VIRTIO_BLK_T_IN;   // 读磁盘
    buf0->reserved = 0;
    buf0->sector   = sector;

    // 请求头描述符 (索引 0)
    // 指向预分配的 disk.ops[] 内存
    // 设置 NEXT 标志和链式指针
    disk.desc[idx[0]].addr  = (uint64)buf0;
    disk.desc[idx[0]].len   = sizeof(struct virtio_blk_req);
    disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
    disk.desc[idx[0]].next  = idx[1];

    // 数据缓冲区描述符 (索引 1)
    // 写操作：设备从内存读取数据 → 磁盘
    // 读操作：设备从磁盘读取数据 → 内存
    // 零拷贝设计：直接使用用户缓冲区的物理地址
    disk.desc[idx[1]].addr = (uint64)b->data;
    disk.desc[idx[1]].len  = BSIZE;
    if (write)
        disk.desc[idx[1]].flags = 0;   // 设备读b->data
    else
        disk.desc[idx[1]].flags = VRING_DESC_F_WRITE;   // 设备写b->data
    disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
    disk.desc[idx[1]].next = idx[2];

    // 状态描述符 (索引 2)
    disk.info[idx[0]].status = 0xff;                                // 初始状态(非0)
    disk.desc[idx[2]].addr   = (uint64)&disk.info[idx[0]].status;   // 状态字段地址
    disk.desc[idx[2]].len    = 1;                                   // 单字节状态
    disk.desc[idx[2]].flags  = VRING_DESC_F_WRITE;                  // 设备可写(返回状态)
    disk.desc[idx[2]].next   = 0;                                   // 链结束

    // 关联请求元数据
    b->disk             = 1;   // 标记缓冲区正在使用
    disk.info[idx[0]].b = b;   // 将缓冲区与描述符关联

    // 告诉设备描述符链中的第一个索引。
    // 添加至可用环
    disk.avail->ring[disk.avail->idx % NUM] = idx[0];

    __sync_synchronize();   // 内存屏障(确保写入顺序)

    // 更新可用索引
    disk.avail->idx += 1;   // not % NUM ...

    __sync_synchronize();   // 再次内存屏障

    *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;   // 通知设备(队列0)

    // 等待磁盘中断表示请求已完成。
    while (b->disk == 1)
    {
        // 在缓冲区上休眠
        sleep(b, &disk.vdisk_lock);
    }

    disk.info[idx[0]].b = 0;     // 清除关联
    free_chain(idx[0]);          // 释放描述符链
    release(&disk.vdisk_lock);   // 释放锁
}

void virtio_disk_intr()
{
    // 获取磁盘操作锁
    acquire(&disk.vdisk_lock);

    // the device won't raise another interrupt until we tell it
    // we've seen this interrupt, which the following line does.
    // this may race with the device writing new entries to
    // the "used" ring, in which case we may process the new
    // completion entries in this interrupt, and have nothing to do
    // in the next interrupt, which is harmless.
    // 读取中断状态寄存器 (VIRTIO_MMIO_INTERRUPT_STATUS)
    // 将状态值写入中断确认寄存器 (VIRTIO_MMIO_INTERRUPT_ACK)
    *R(VIRTIO_MMIO_INTERRUPT_ACK) = *R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;

    __sync_synchronize();

    // the device increments disk.used->idx when it
    // adds an entry to the used ring.

    while (disk.used_idx != disk.used->idx)
    {
        __sync_synchronize();
        // 获取完成项ID
        int id = disk.used->ring[disk.used_idx % NUM].id;
        // 检查状态字节
        if (disk.info[id].status != 0)
            panic("virtio_disk_intr status");


        struct buf* b = disk.info[id].b;   // 获取关联缓冲区
        b->disk       = 0;                 // 设置 b->disk = 0 标记操作完成
        wakeup(b);                         // wakeup(b) 唤醒在缓冲区上睡眠的进程

        disk.used_idx += 1;   // 更新索引
    }

    release(&disk.vdisk_lock);
}
