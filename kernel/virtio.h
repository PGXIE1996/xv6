//
// virtio device definitions.
// for both the mmio interface, and virtio descriptors.
// only tested with qemu.
//
// the virtio spec:
// https://docs.oasis-open.org/virtio/virtio/v1.1/virtio-v1.1.pdf
//

// virtio mmio control registers, mapped starting at 0x10001000.
// from qemu virtio_mmio.h
// VirtIO MMIO 控制寄存器
#define VIRTIO_MMIO_MAGIC_VALUE      0x000   // 读取时返回 0x74726976:用于验证设备是否为 VirtIO 设备
#define VIRTIO_MMIO_VERSION          0x004   // 表示 VirtIO 协议版本，xv6 期望值为 2
#define VIRTIO_MMIO_DEVICE_ID        0x008   // 设备类型标识，1 表示网络设备，2 表示块设备
#define VIRTIO_MMIO_VENDOR_ID        0x00c   // 供应商 ID，QEMU 的值为 0x554d4551
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010   // 设备支持的功能位图（只读），由设备提供
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020   // 驱动程序支持的功能位图（只写），由驱动程序设置。
#define VIRTIO_MMIO_QUEUE_SEL        0x030   // 选择要操作的队列（只写）
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034   // 当前队列支持的最大描述符数量（只读）。
#define VIRTIO_MMIO_QUEUE_NUM        0x038   // 驱动程序设置的当前队列的描述符数量（只写）。
#define VIRTIO_MMIO_QUEUE_READY      0x044   // 队列就绪标志，写入 1 表示队列已初始化并可使用。
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050   // 写入队列索引，通知设备有新的描述符可用。
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060   // 中断状态寄存器，指示设备触发了哪些中断（只读）。
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064   // 写入中断位，确认已处理的中断（只写）。
#define VIRTIO_MMIO_STATUS           0x070   // 设备状态寄存器，驱动程序通过读写此寄存器与设备协商初始化过程。
#define VIRTIO_MMIO_QUEUE_DESC_LOW   0x080   // 描述符表的物理地址（只写）。
#define VIRTIO_MMIO_QUEUE_DESC_HIGH  0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW  0x090   // 可用环（available ring）的物理地址（只写）。
#define VIRTIO_MMIO_DRIVER_DESC_HIGH 0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW  0x0a0   // 已使用环（used ring）的物理地址（只写）。
#define VIRTIO_MMIO_DEVICE_DESC_HIGH 0x0a4

// status register bits, from qemu virtio_config.h
// 状态寄存器位：用于驱动程序和设备之间的初始化协商。
#define VIRTIO_CONFIG_S_ACKNOWLEDGE        (1 << 0)   // 驱动检测到设备
#define VIRTIO_CONFIG_S_DRIVER             (1 << 1)   // 驱动已加载并准备初始化
#define VIRTIO_CONFIG_S_DRIVER_OK          (1 << 2)   // 驱动完成初始化，设备可正常工作
#define VIRTIO_CONFIG_S_FEATURES_OK        (1 << 3)   // 功能协商完成，设备和驱动达成一致
#define VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET (1 << 6)   // 设备发生错误需要重置
#define VIRTIO_CONFIG_S_FAILED             (1 << 7)   // 初始化失败

// device feature bits
// 设备功能位
#define VIRTIO_BLK_F_RO             5    // 磁盘为只读模式。
#define VIRTIO_BLK_F_SCSI           7    // 支持 SCSI 命令直通（允许直接发送 SCSI 命令到设备）。
#define VIRTIO_BLK_F_CONFIG_WCE     11   // 支持配置写回模式（writeback caching）。
#define VIRTIO_BLK_F_MQ             12   // 支持多个虚拟队列（multi-queue）。
#define VIRTIO_F_ANY_LAYOUT         27   // 允许非标准的数据布局（灵活的描述符格式）。
#define VIRTIO_RING_F_INDIRECT_DESC 28   // 支持间接描述符表（优化内存使用）。
#define VIRTIO_RING_F_EVENT_IDX     29   // 支持事件索引，优化中断处理。

// this many virtio descriptors.
// must be a power of two.
// 定义了虚拟队列中描述符的数量为 8，且要求是 2 的幂
#define NUM 8

// a single descriptor, from the spec.
// 描述符结构
struct virtq_desc
{
    uint64 addr;    // 缓冲区物理地址
    uint32 len;     // 缓冲区长度（字节）
    uint16 flags;   // 标志（如 VIRTIO_DESC_F_NEXT, VIRTIO_DESC_F_WRITE）
    uint16 next;    // 下一个描述符索引（用于链式描述符）
};
#define VRING_DESC_F_NEXT  1   // 表示此描述符后还有下一个描述符
#define VRING_DESC_F_WRITE 2   // 表示设备将数据写入此缓冲区（否则驱动程序提供数据给设备读取）。

// the (entire) avail ring, from the spec.
// 可用环结构
struct virtq_avail
{
    uint16 flags;       // 标志（如通知抑制）
    uint16 idx;         // 当前索引，指示下一个可用位置
    uint16 ring[NUM];   // 描述符索引数组（NUM 为队列大小）
    uint16 unused;      // 用于事件索引通知（未启用）
};

// one entry in the "used" ring, with which the
// device tells the driver about completed requests.
// 已使用环元素
struct virtq_used_elem
{
    uint32 id;    // 描述符链的起始索引
    uint32 len;   // 写入的字节数（对于读请求）
};

// 已使用环结构
struct virtq_used
{
    uint16                 flags;       // 标志（如通知抑制）
    uint16                 idx;         // 当前索引，指示下一个已用位置
    struct virtq_used_elem ring[NUM];   // 已用描述符数组
};

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN  0   // 读取磁盘
#define VIRTIO_BLK_T_OUT 1   // 写入磁盘

// the format of the first descriptor in a disk request.
// to be followed by two more descriptors containing
// the block, and a one-byte status.
// 块设备请求格式
struct virtio_blk_req
{
    uint32 type;       // 读取磁盘or写入磁盘
    uint32 reserved;   // 保留
    uint64 sector;     // 磁盘的扇区号
};
