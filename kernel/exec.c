#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"

static int loadseg(pde_t*, uint64, struct inode*, uint, uint);

// 将ELF文件程序段的标志（flags）转换为页面表条目（PTE）的权限位
// flags是从ELF程序头（proghdr）中读取的标志位
int flags2perm(int flags)
{
    int perm = 0;
    if (flags & 0x1)
        perm = PTE_X;
    if (flags & 0x2)
        perm |= PTE_W;
    return perm;
}

// 实现exec系统调用，加载并执行指定路径的可执行文件，并传递参数argv
int exec(char* path, char** argv)
{
    char *s, *last;   // s, last：用于解析程序名称。
    int   i, off;     // 循环计数器和文件偏移量。
    // argc：参数个数
    // sz：新程序的内存大小，逐步累加
    // sp：用户栈指针
    // ustack[MAXARG]：存储用户栈中参数字符串的地址
    // stackbase：用户栈底地址
    uint64         argc, sz = 0, sp, ustack[MAXARG], stackbase;
    struct elfhdr  elf;                           // ELF文件头结构
    struct inode*  ip;                            // 指向可执行文件的索引节点
    struct proghdr ph;                            // ELF程序头结构
    pagetable_t    pagetable = 0, oldpagetable;   // 新页表与旧页表
    struct proc*   p         = myproc();          // 当前进程结构

    begin_op();
    // 查找文件：调用namei(path)解析路径，获取可执行文件的索引节点ip
    if ((ip = namei(path)) == 0)
    {
        end_op();
        return -1;
    }
    // 锁定文件：ilock(ip)锁定索引节点，防止并发修改
    ilock(ip);

    // 调用readi从文件偏移0读取ELF文件头到elf结构
    if (readi(ip, 0, (uint64)&elf, 0, sizeof(elf)) != sizeof(elf))
        goto bad;
    // 检查elf.magic是否等于ELF_MAGIC
    if (elf.magic != ELF_MAGIC)
        goto bad;
    // 调用proc_pagetable(p)为进程创建新的页面表，存储到pagetable
    if ((pagetable = proc_pagetable(p)) == 0)
        goto bad;

    // 加载程序段到内存
    // 循环读取ELF文件的程序头表
    for (i = 0, off = elf.phoff; i < elf.phnum; i++, off += sizeof(ph))
    {
        // 调用readi读取每个程序头到ph
        if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
            goto bad;
        // 检查程序头类型
        if (ph.type != ELF_PROG_LOAD)
            continue;
        // 验证大小和地址
        if (ph.memsz < ph.filesz)
            goto bad;
        if (ph.vaddr + ph.memsz < ph.vaddr)
            goto bad;
        if (ph.vaddr % PGSIZE != 0)
            goto bad;
        // 调用uvmalloc为程序段分配内存，从当前sz到ph.vaddr + ph.memsz
        // 权限由flags2perm(ph.flags)设置
        uint64 sz1;
        if ((sz1 = uvmalloc(pagetable, sz, ph.vaddr + ph.memsz, flags2perm(ph.flags))) == 0)
            goto bad;
        sz = sz1;
        // 调用loadseg将程序段从文件加载到内存
        if (loadseg(pagetable, ph.vaddr, ip, ph.off, ph.filesz) < 0)
            goto bad;
    }
    // 释放索引节点
    iunlockput(ip);
    end_op();
    ip = 0;
    // 分配用户栈
    p            = myproc();
    uint64 oldsz = p->sz;

    // 分配栈空间 (2页)
    sz = PGROUNDUP(sz);
    uint64 sz1;
    if ((sz1 = uvmalloc(pagetable, sz, sz + 2 * PGSIZE, PTE_W)) == 0)
        goto bad;
    sz = sz1;
    // 设置栈保护页
    uvmclear(pagetable, sz - 2 * PGSIZE);
    sp        = sz;
    stackbase = sp - PGSIZE;   // 实际栈空间

    // 推送参数到用户栈
    for (argc = 0; argv[argc]; argc++)
    {
        if (argc >= MAXARG)
            goto bad;
        sp -= strlen(argv[argc]) + 1;
        sp -= sp % 16;   // 16字节对齐
        if (sp < stackbase)
            goto bad;
        if (copyout(pagetable, sp, argv[argc], strlen(argv[argc]) + 1) < 0)
            goto bad;
        ustack[argc] = sp;   // 记录参数指针
    }
    ustack[argc] = 0;   // 结束标记

    // 推送argv指针数组
    sp -= (argc + 1) * sizeof(uint64);
    sp -= sp % 16;
    if (sp < stackbase)
        goto bad;
    if (copyout(pagetable, sp, (char*)ustack, (argc + 1) * sizeof(uint64)) < 0)
        goto bad;

    // 设置用户程序参数
    p->trapframe->a1 = sp;

    // 保存程序名称
    for (last = s = path; *s; s++)
        if (*s == '/')
            last = s + 1;
    safestrcpy(p->name, last, sizeof(p->name));

    // 提交新程序映像
    oldpagetable      = p->pagetable;          // 保存旧页表
    p->pagetable      = pagetable;             // 替换页面表
    p->sz             = sz;                    // 更新内存大小
    p->trapframe->epc = elf.entry;             // 设置程序入口
    p->trapframe->sp  = sp;                    // 设置栈指针
    proc_freepagetable(oldpagetable, oldsz);   // 释放旧页面表和内存。

    return argc;   // this ends up in a0, the first argument to main(argc, argv)

bad:
    if (pagetable)
        proc_freepagetable(pagetable, sz);
    if (ip)
    {
        iunlockput(ip);
        end_op();
    }
    return -1;
}

// 将程序段从文件加载到指定虚拟地址的页面表中。
static int loadseg(pagetable_t pagetable, uint64 va, struct inode* ip, uint offset, uint sz)
{
    uint   i, n;
    uint64 pa;

    for (i = 0; i < sz; i += PGSIZE)
    {
        pa = walkaddr(pagetable, va + i);
        if (pa == 0)
            panic("loadseg: address should exist");
        if (sz - i < PGSIZE)
            n = sz - i;
        else
            n = PGSIZE;
        if (readi(ip, 0, (uint64)pa, offset + i, n) != n)
            return -1;
    }

    return 0;
}
