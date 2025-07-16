#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64 sys_exit(void)
{
    int n;
    argint(0, &n);   // 从a0寄存器获取退出状态码
    exit(n);         // 调用进程终止函数
    return 0;        // 实际不会执行到此处
}

uint64 sys_getpid(void)
{
    return myproc()->pid;   // 直接访问当前进程PCB
}

uint64 sys_fork(void)
{
    return fork();   // 此函数用于处理 fork 系统调用
}

// 该函数用于处理 wait 系统调用
uint64 sys_wait(void)
{
    uint64 p;
    argaddr(0, &p);
    return wait(p);
}

uint64 sys_sbrk(void)
{
    uint64 addr;
    int    n;

    argint(0, &n);
    addr = myproc()->sz;
    if (growproc(n) < 0)
        return -1;
    return addr;
}

uint64 sys_sleep(void)
{
    int  n;
    uint ticks0;

    // 从当前进程的陷阱帧中获取第0个系统调用参数
    // 将参数值存入变量 n 中（表示要休眠的时钟滴答数）
    argint(0, &n);
    acquire(&tickslock);
    ticks0 = ticks;
    while (ticks - ticks0 < n)
    {
        if (killed(myproc()))
        {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

uint64 sys_kill(void)
{
    int pid;

    argint(0, &pid);
    return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void)
{
    uint xticks;

    acquire(&tickslock);
    xticks = ticks;
    release(&tickslock);
    return xticks;
}
