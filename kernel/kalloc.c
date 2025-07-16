// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"


/********************************************************
整体工作流程
系统启动时，kinit()初始化内存分配器，将内核之后的物理内存标记为可用
当需要内存时（如创建页表、分配进程栈），调用kalloc()获取一页物理内存
当内存不再需要时，调用kfree()将内存页返回给分配器
分配器使用空闲链表管理可用内存页，采用头插法实现快速分配和释放
*********************************************************/

void freerange(void* pa_start, void* pa_end);

extern char end[];   // first address after kernel. defined by kernel.ld.

struct run
{
    struct run* next;
};

struct
{
    struct spinlock lock;
    struct run*     freelist;
} kmem;

// 初始化内存分配器
void kinit()
{
    initlock(&kmem.lock, "kmem");
    freerange(end, (void*)PHYSTOP);
}

// 将指定范围内的物理内存页按页大小对齐后，逐个释放到空闲链表中
void freerange(void* pa_start, void* pa_end)
{
    char* p;
    p = (char*)PGROUNDUP((uint64)pa_start);
    for (; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
        kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
// 内存释放函数：负责将不再使用的内存页添加到空闲链表中
// 将所有数据填充位1
void kfree(void* pa)
{
    struct run* r;

    if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
        panic("kfree");

    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);

    r = (struct run*)pa;

    acquire(&kmem.lock);
    r->next       = kmem.freelist;
    kmem.freelist = r;
    release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// 内内存分配函数：负责从空闲链表中获取一个可用内存页
void* kalloc(void)
{
    struct run* r;

    acquire(&kmem.lock);
    r = kmem.freelist;
    if (r)
        kmem.freelist = r->next;
    release(&kmem.lock);

    if (r)
        memset((char*)r, 5, PGSIZE);   // fill with junk
    return (void*)r;
}
