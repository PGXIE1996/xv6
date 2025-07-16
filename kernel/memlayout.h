// Physical memory layout

// qemu -machine virt is set up like this,
// based on qemu's hw/riscv/virt.c:
//
// 00001000 -- boot ROM, provided by qemu
// 02000000 -- CLINT
// 0C000000 -- PLIC
// 10000000 -- uart0
// 10001000 -- virtio disk
// 80000000 -- boot ROM jumps here in machine mode
//             -kernel loads the kernel here
// unused RAM after 80000000.

// the kernel uses physical memory thus:
// 80000000 -- entry.S, then kernel text and data
// end -- start of kernel page allocation area
// PHYSTOP -- end RAM used by the kernel

// qemu puts UART registers here in physical memory.
#define UART0     0x10000000L
#define UART0_IRQ 10

// virtio mmio interface
#define VIRTIO0     0x10001000L
#define VIRTIO0_IRQ 1

// core local interruptor (CLINT), which contains the timer.
#define CLINT                  0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8 * (hartid))
#define CLINT_MTIME            (CLINT + 0xBFF8)   // cycles since boot.

// qemu puts platform-level interrupt controller (PLIC) here.
// 定义 PLIC 的基地址。
#define PLIC 0x0c000000L
// 定义中断源的优先级寄存器（Priority Registers）的起始地址。
#define PLIC_PRIORITY (PLIC + 0x0)
// 定义中断挂起寄存器（Pending Registers）的起始地址。只读
#define PLIC_PENDING (PLIC + 0x1000)
// 定义机器模式下每个 CPU 核心的中断使能寄存器（Enable Registers）的起始地址。
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart) * 0x100)
// 定义监督者模式下每个 CPU 核心的中断使能寄存器（Enable Registers）的起始地址。
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart) * 0x100)
// 定义机器模式下每个 CPU 核心的中断优先级阈值寄存器（Threshold Register）的地址。
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart) * 0x2000)
// 定义监督者模式下每个 CPU 核心的中断优先级阈值寄存器（Threshold Register）的地址。
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart) * 0x2000)
// 定义机器模式下每个 CPU 核心的声明/完成寄存器（Claim/Complete Register）的地址。
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart) * 0x2000)
// 定义监督者模式下每个 CPU 核心的声明/完成寄存器（Claim/Complete Register）的地址。
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart) * 0x2000)

// the kernel expects there to be RAM
// for use by the kernel and user pages
// from physical address 0x80000000 to PHYSTOP.
#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// map the trampoline page to the highest address,
// in both user and kernel space.
#define TRAMPOLINE (MAXVA - PGSIZE)

// map kernel stacks beneath the trampoline,
// each surrounded by invalid guard pages.
#define KSTACK(p) (TRAMPOLINE - ((p) + 1) * 2 * PGSIZE)

// User memory layout.
// Address zero first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
//   ...
//   TRAPFRAME (p->trapframe, used by the trampoline)
//   TRAMPOLINE (the same page as in the kernel)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
