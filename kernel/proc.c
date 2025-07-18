#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// CPU信息表
struct cpu cpus[NCPU];

// 进程表
struct proc proc[NPROC];

// 指向初始用户进程
struct proc* initproc;

int             nextpid = 1;   // 进程ID
struct spinlock pid_lock;      // 自旋锁

extern void forkret(void);
static void freeproc(struct proc* p);

extern char trampoline[];   // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
// 保护父进程等待子进程时的同步，防止竞争条件
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

// 为每个进程分配内核栈，并将其映射到内核页表。
void proc_mapstacks(pagetable_t kpgtbl)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        char* pa = kalloc();
        if (pa == 0)
            panic("kalloc");
        uint64 va = KSTACK((int)(p - proc));
        kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
    }
}

// initialize the proc table.
// 进程表初始化，绑定栈的(内核)虚拟地址
void procinit(void)
{
    struct proc* p;

    initlock(&pid_lock, "nextpid");
    initlock(&wait_lock, "wait_lock");
    for (p = proc; p < &proc[NPROC]; p++)
    {
        initlock(&p->lock, "proc");
        p->state = UNUSED;
        // 绑定进程栈的虚拟地址
        p->kstack = KSTACK((int)(p - proc));
    }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
// 获取 CPU ID
int cpuid()
{
    int id = r_tp();
    return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
// 获取CPU结构体
struct cpu* mycpu(void)
{
    int         id = cpuid();
    struct cpu* c  = &cpus[id];
    return c;
}

// Return the current struct proc *, or zero if none.
// 获取当前进程
struct proc* myproc(void)
{
    push_off();
    struct cpu*  c = mycpu();
    struct proc* p = c->proc;
    pop_off();
    return p;
}

// 分配进程 ID
int allocpid()
{
    int pid;

    acquire(&pid_lock);
    pid     = nextpid;
    nextpid = nextpid + 1;
    release(&pid_lock);

    return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
// 分配进程：从进程表中找一个未使用的进程槽，初始化其基本状态并返回。
static struct proc* allocproc(void)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->state == UNUSED)
        {
            goto found;
        }
        else
        {
            release(&p->lock);
        }
    }
    return 0;

found:
    p->pid   = allocpid();
    p->state = USED;

    // Allocate a trapframe page.
    if ((p->trapframe = (struct trapframe*)kalloc()) == 0)
    {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // An empty user page table.
    p->pagetable = proc_pagetable(p);
    if (p->pagetable == 0)
    {
        freeproc(p);
        release(&p->lock);
        return 0;
    }

    // Set up new context to start executing at forkret,
    // which returns to user space.

    memset(&p->context, 0, sizeof(p->context));
    p->context.ra = (uint64)forkret;
    p->context.sp = p->kstack + PGSIZE;

    return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
// 释放进程：释放进程的资源并将其标记为未使用。
static void freeproc(struct proc* p)
{
    if (p->trapframe)
        kfree((void*)p->trapframe);
    p->trapframe = 0;
    if (p->pagetable)
        proc_freepagetable(p->pagetable, p->sz);
    p->pagetable = 0;
    p->sz        = 0;
    p->pid       = 0;
    p->parent    = 0;
    p->name[0]   = 0;
    p->chan      = 0;
    p->killed    = 0;
    p->xstate    = 0;
    p->state     = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
// 为进程创建用户页表，映射 trampoline 和 trapframe。
pagetable_t proc_pagetable(struct proc* p)
{
    pagetable_t pagetable;

    // An empty page table.
    // 获取一个空的进程页表（第一页）
    pagetable = uvmcreate();
    if (pagetable == 0)
        return 0;

    // 映射trampoline到用户地址空间最高页TRAMPOLINE
    if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline, PTE_R | PTE_X) < 0)
    {
        uvmfree(pagetable, 0);
        return 0;
    }

    // map the trapframe page just below the trampoline page, for
    // trampoline.S.
    // 映射p->trapframe到用户地址空间次高页TRAPFRAME
    if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
    {
        uvmunmap(pagetable, TRAMPOLINE, 1, 0);
        uvmfree(pagetable, 0);
        return 0;
    }

    return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
// 释放进程的页表和相关物理内存。
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
    // 解除虚拟地址TRAMPOLINE的物理映射
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    // 解除虚拟地址TRAPFRAME的物理映射
    uvmunmap(pagetable, TRAPFRAME, 1, 0);
    // 解除最后一级页表的映射并释放对应的物理内存
    // 递归释放页表及其子页表
    uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode

uchar initcode[] = {0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02, 0x97, 0x05, 0x00, 0x00, 0x93,
                    0x85, 0x35, 0x02, 0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00, 0x93, 0x08,
                    0x20, 0x00, 0x73, 0x00, 0x00, 0x00, 0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e,
                    0x69, 0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Set up first user process.
// 创建并初始化第一个用户进程（运行 /init 程序）。
void userinit(void)
{
    struct proc* p;

    p        = allocproc();
    initproc = p;

    // allocate one user page and copy initcode's instructions
    // and data into it.
    uvmfirst(p->pagetable, initcode, sizeof(initcode));
    p->sz = PGSIZE;

    // prepare for the very first "return" from kernel to user.
    p->trapframe->epc = 0;        // user program counter，用户程序入口地址（0x0）
    p->trapframe->sp  = PGSIZE;   // user stack pointer，用户栈指针（地址0x1000）
    safestrcpy(p->name, "initcode", sizeof(p->name));   // 标记为可运行状态
    p->cwd   = namei("/");                              // 当前工作目录设为根目录
    p->state = RUNNABLE;                                // 标记为可运行状态

    release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
// 增加或减少进程的用户内存大小。
int growproc(int n)
{
    uint64       sz;
    struct proc* p = myproc();

    sz = p->sz;
    if (n > 0)
    {
        if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
        {
            return -1;
        }
    }
    else if (n < 0)
    {
        sz = uvmdealloc(p->pagetable, sz, sz + n);
    }
    p->sz = sz;
    return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
// 创建子进程，复制父进程的内存和状态。
int fork(void)
{
    int          i, pid;
    struct proc* np;
    struct proc* p = myproc();

    // Allocate process.
    if ((np = allocproc()) == 0)
    {
        return -1;
    }

    // Copy user memory from parent to child.
    // uvmcopy：复制父进程的页表和物理内存到子进程（用于 fork）。
    if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
    {
        freeproc(np);
        release(&np->lock);
        return -1;
    }
    np->sz = p->sz;

    // copy saved user registers.
    *(np->trapframe) = *(p->trapframe);

    // Cause fork to return 0 in the child.
    np->trapframe->a0 = 0;

    // increment reference counts on open file descriptors.

    for (i = 0; i < NOFILE; i++)
        if (p->ofile[i])
            np->ofile[i] = filedup(p->ofile[i]);
    np->cwd = idup(p->cwd);


    safestrcpy(np->name, p->name, sizeof(p->name));

    pid = np->pid;
    // 先释放进程私有锁，因为设置parent需要wait_lock,防止构成死锁
    release(&np->lock);

    // 将新创建的子进程（np）的 parent 字段设置为当前进程
    /*
    ****************************************************************
    ** wait_lock 保护所有涉及父子关系的关键代码，包括：
    **      fork 中的 np->parent 设置。
    **      wait 中的子进程遍历和状态检查。
    **      exit 中的父进程通知和 reparent 操作。
    ** 通过全局锁，确保这些操作序列化，避免竞争。
    ****************************************************************
    */
    acquire(&wait_lock);
    np->parent = p;
    release(&wait_lock);


    acquire(&np->lock);
    np->state = RUNNABLE;
    release(&np->lock);

    return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
// 在进程 p 退出时，确保其所有子进程不会成为无人管理的孤儿进程。
// 通过将子进程的父进程重新设置为 initproc，并唤醒 initproc 来处理这些子进程。
void reparent(struct proc* p)
{
    struct proc* pp;

    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
        if (pp->parent == p)
        {
            pp->parent = initproc;
            // 为了通知 initproc 检查并回收可能的 ZOMBIE 状态子进程？
            wakeup(initproc);
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
// 终止当前进程，进入僵尸状态（ZOMBIE），等待父进程回收。
void exit(int status)
{
    struct proc* p = myproc();

    if (p == initproc)
        panic("init exiting");

    // Close all open files.
    for (int fd = 0; fd < NOFILE; fd++)
    {
        if (p->ofile[fd])
        {
            struct file* f = p->ofile[fd];
            fileclose(f);
            p->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(p->cwd);
    end_op();
    p->cwd = 0;

    acquire(&wait_lock);

    // Give any children to init.
    reparent(p);

    // Parent might be sleeping in wait().
    wakeup(p->parent);

    acquire(&p->lock);

    p->xstate = status;
    p->state  = ZOMBIE;

    release(&wait_lock);

    // Jump into the scheduler, never to return.
    sched();
    panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
// 父进程等待子进程退出，返回子进程的 PID 和退出状态。
// addr：用户空间地址，用于接收子进程的退出状态码（exit status）
int wait(uint64 addr)
{
    struct proc* pp;
    int          havekids, pid;
    struct proc* p = myproc();

    acquire(&wait_lock);

    for (;;)
    {
        // 遍历所有进程
        havekids = 0;
        for (pp = proc; pp < &proc[NPROC]; pp++)
        {
            if (pp->parent == p)
            {
                // make sure the child isn't still in exit() or swtch().
                acquire(&pp->lock);

                havekids = 1;
                if (pp->state == ZOMBIE)
                {
                    // 记录子进程PID
                    pid = pp->pid;
                    // 将子进程退出信息（pp->xstate,在内核中）复制到addr对应的父进程用户空间中（由用户态参数传递）
                    if (addr != 0 &&
                        copyout(p->pagetable, addr, (char*)&pp->xstate, sizeof(pp->xstate)) < 0)
                    {
                        release(&pp->lock);
                        release(&wait_lock);
                        return -1;
                    }
                    // 1.释放trampframe物理页表
                    // 2.调用proc_freepagetable释放虚拟地址对应的物理内存一以及物理页表
                    freeproc(pp);
                    release(&pp->lock);
                    release(&wait_lock);
                    return pid;
                }
                release(&pp->lock);
            }
        }

        // No point waiting if we don't have any children.
        if (!havekids || killed(p))
        {
            release(&wait_lock);
            return -1;
        }

        // channel：p，在进程p上睡眠
        // 传入wait_lock:进入睡眠后释放全局锁wait_lock
        sleep(p, &wait_lock);   // DOC: wait-sleep
    }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
// 每个 CPU 运行的调度器，循环选择可运行进程并切换到它。
void scheduler(void)
{
    struct proc* p;
    struct cpu*  c = mycpu();

    c->proc = 0;
    for (;;)
    {
        // Avoid deadlock by ensuring that devices can interrupt.
        intr_on();

        for (p = proc; p < &proc[NPROC]; p++)
        {
            acquire(&p->lock);
            if (p->state == RUNNABLE)
            {
                // Switch to chosen process.  It is the process's job
                // to release its lock and then reacquire it
                // before jumping back to us.
                p->state = RUNNING;
                c->proc  = p;
                swtch(&c->context, &p->context);

                // Process is done running for now.
                // It should have changed its p->state before coming back.
                c->proc = 0;
            }
            release(&p->lock);
        }
    }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
// 将当前进程切换到调度器
void sched(void)
{
    int          intena;
    struct proc* p = myproc();

    if (!holding(&p->lock))
        panic("sched p->lock");
    if (mycpu()->noff != 1)
        panic("sched locks");
    if (p->state == RUNNING)
        panic("sched running");
    if (intr_get())
        panic("sched interruptible");

    intena = mycpu()->intena;

    swtch(&p->context, &mycpu()->context);
    mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// 当前进程主动让出 CPU，进入可运行状态
void yield(void)
{
    struct proc* p = myproc();
    acquire(&p->lock);
    p->state = RUNNABLE;
    sched();
    release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
// 在进程从内核态首次返回用户态时执行，确保正确初始化并切换到用户态
void forkret(void)
{
    static int first = 1;

    // Still holding p->lock from scheduler.
    release(&myproc()->lock);

    if (first)
    {
        // File system initialization must be run in the context of a
        // regular process (e.g., because it calls sleep), and thus cannot
        // be run from main().
        first = 0;
        fsinit(ROOTDEV);
    }

    usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
// 使当前进程在指定通道（chan）上休眠，等待被唤醒
void sleep(void* chan, struct spinlock* lk)
{
    struct proc* p = myproc();

    // Must acquire p->lock in order to
    // change p->state and then call sched.
    // Once we hold p->lock, we can be
    // guaranteed that we won't miss any wakeup
    // (wakeup locks p->lock),
    // so it's okay to release lk.

    acquire(&p->lock);   // DOC: sleeplock1
    release(lk);

    // Go to sleep.
    p->chan  = chan;
    p->state = SLEEPING;

    sched();

    // Tidy up.
    p->chan = 0;

    // Reacquire original lock.
    release(&p->lock);
    acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
// 唤醒在指定通道（chan）上休眠的所有进程
void wakeup(void* chan)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        if (p != myproc())
        {
            acquire(&p->lock);
            if (p->state == SLEEPING && p->chan == chan)
            {
                p->state = RUNNABLE;
            }
            release(&p->lock);
        }
    }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
// 标记指定 PID 的进程为 killed，使其在下次返回用户态时退出
int kill(int pid)
{
    struct proc* p;

    for (p = proc; p < &proc[NPROC]; p++)
    {
        acquire(&p->lock);
        if (p->pid == pid)
        {
            p->killed = 1;
            if (p->state == SLEEPING)
            {
                // Wake process from sleep().
                p->state = RUNNABLE;
            }
            release(&p->lock);
            return 0;
        }
        release(&p->lock);
    }
    return -1;
}

// 将指定进程的 killed 字段设为 1，标记其为“被杀死”状态。
void setkilled(struct proc* p)
{
    acquire(&p->lock);
    p->killed = 1;
    release(&p->lock);
}

// 检查进程的killed值
int killed(struct proc* p)
{
    int k;

    acquire(&p->lock);
    k = p->killed;
    release(&p->lock);
    return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
// 用于将数据从内核空间复制到目标地址（可以是用户空间或内核空间）
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len)
{
    struct proc* p = myproc();
    if (user_dst)
    {
        return copyout(p->pagetable, dst, src, len);
    }
    else
    {
        memmove((char*)dst, src, len);
        return 0;
    }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
// 将数据从用户空间或内核空间的源地址复制到内核空间的目标地址
int either_copyin(void* dst, int user_src, uint64 src, uint64 len)
{
    struct proc* p = myproc();
    if (user_src)
    {
        return copyin(p->pagetable, dst, src, len);
    }
    else
    {
        memmove(dst, (char*)src, len);
        return 0;
    }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
// 打印进程表中所有非 UNUSED 进程的信息，用于调试。
void procdump(void)
{
    static char* states[] = {[UNUSED] "unused",   [USED] "used",      [SLEEPING] "sleep ",
                             [RUNNABLE] "runble", [RUNNING] "run   ", [ZOMBIE] "zombie"};
    struct proc* p;
    char*        state;

    printf("\n");
    for (p = proc; p < &proc[NPROC]; p++)
    {
        if (p->state == UNUSED)
            continue;
        if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
            state = states[p->state];
        else
            state = "???";
        printf("%d %s %s", p->pid, state, p->name);
        printf("\n");
    }
}
