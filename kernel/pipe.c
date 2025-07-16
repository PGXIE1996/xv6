#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"

// 定义管道缓冲区大小为 512 字节，限制管道一次可存储的数据量
#define PIPESIZE 512

// 定义管道的内存数据结构，用于存储数据和状态。
struct pipe
{
    struct spinlock lock;             // 自旋锁，保护管道数据
    char            data[PIPESIZE];   // 管道缓冲区，固定大小
    uint            nread;            // 已读取的字节数
    uint            nwrite;           // 已写入的字节数
    int             readopen;         // 读端是否仍打开（1=打开，0=关闭）
    int             writeopen;        // 写端是否仍打开（1=打开，0=关闭）
};

// 创建管道，分配读端和写端的文件描述符（f0 和 f1），并初始化管道结构。
int pipealloc(struct file** f0, struct file** f1)
{
    struct pipe* pi;

    pi  = 0;
    *f0 = *f1 = 0;
    if ((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
        goto bad;
    if ((pi = (struct pipe*)kalloc()) == 0)
        goto bad;
    pi->readopen  = 1;
    pi->writeopen = 1;
    pi->nwrite    = 0;
    pi->nread     = 0;
    initlock(&pi->lock, "pipe");
    (*f0)->type     = FD_PIPE;
    (*f0)->readable = 1;
    (*f0)->writable = 0;
    (*f0)->pipe     = pi;
    (*f1)->type     = FD_PIPE;
    (*f1)->readable = 0;
    (*f1)->writable = 1;
    (*f1)->pipe     = pi;
    return 0;

bad:
    if (pi)
        kfree((char*)pi);
    if (*f0)
        fileclose(*f0);
    if (*f1)
        fileclose(*f1);
    return -1;
}

// 关闭管道的读端或写端，当两端都关闭时释放管道。
void pipeclose(struct pipe* pi, int writable)
{
    acquire(&pi->lock);
    if (writable)
    {
        pi->writeopen = 0;
        // 唤醒等待读取的进程（因为写端关闭，读端可能收到 EOF）。
        wakeup(&pi->nread);
    }
    else
    {
        pi->readopen = 0;
        // 唤醒等待写入的进程（因为读端关闭，写端可能报错）。
        wakeup(&pi->nwrite);
    }
    if (pi->readopen == 0 && pi->writeopen == 0)
    {
        release(&pi->lock);
        kfree((char*)pi);
    }
    else
        release(&pi->lock);
}

// 从用户空间地址 addr 向管道写入 n 个字节。
int pipewrite(struct pipe* pi, uint64 addr, int n)
{
    int          i  = 0;
    struct proc* pr = myproc();

    acquire(&pi->lock);
    while (i < n)
    {
        // （读端关闭，管道断开）
        if (pi->readopen == 0 || killed(pr))
        {
            release(&pi->lock);
            return -1;
        }
        if (pi->nwrite == pi->nread + PIPESIZE)
        {   // DOC: pipewrite-full
            wakeup(&pi->nread);
            sleep(&pi->nwrite, &pi->lock);
        }
        else
        {
            char ch;
            // 使用 copyin 从用户地址 addr + i 复制 1 字节到 ch。
            if (copyin(pr->pagetable, &ch, addr + i, 1) == -1)
                break;
            pi->data[pi->nwrite++ % PIPESIZE] = ch;
            i++;
        }
    }
    wakeup(&pi->nread);
    release(&pi->lock);

    return i;
}

// 从管道读取最多 n 个字节到用户空间地址 addr。
int piperead(struct pipe* pi, uint64 addr, int n)
{
    int          i;
    struct proc* pr = myproc();
    char         ch;

    acquire(&pi->lock);
    // 如果管道为空且写端仍打开，调用sleep
    while (pi->nread == pi->nwrite && pi->writeopen)
    {   // DOC: pipe-empty
        if (killed(pr))
        {
            release(&pi->lock);
            return -1;
        }
        sleep(&pi->nread, &pi->lock);   // DOC: piperead-sleep
    }
    for (i = 0; i < n; i++)
    {   //无数据则退出循环。
        if (pi->nread == pi->nwrite)
            break;
        // 使用 copyout 将 ch 复制到用户地址 addr + i。
        ch = pi->data[pi->nread++ % PIPESIZE];
        if (copyout(pr->pagetable, addr + i, &ch, 1) == -1)
            break;
    }
    wakeup(&pi->nwrite);   // DOC: piperead-wakeup
    release(&pi->lock);
    return i;
}
