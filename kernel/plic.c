#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"

//
// the riscv Platform Level Interrupt Controller (PLIC).
//
// 全局中断优先级初始化
void plicinit(void)
{
    // set desired IRQ priorities non-zero (otherwise disabled).
    *(uint32*)(PLIC + UART0_IRQ * 4)   = 1;   // UART中断优先级设为1
    *(uint32*)(PLIC + VIRTIO0_IRQ * 4) = 1;   // 磁盘中断优先级设为1
}

// 核心相关中断配置
void plicinithart(void)
{
    int hart = cpuid();

    // set enable bits for this hart's S-mode
    // for the uart and virtio disk.
    // 打开当前核心的UART和磁盘中断
    *(uint32*)PLIC_SENABLE(hart) = (1 << UART0_IRQ) | (1 << VIRTIO0_IRQ);

    // set this hart's S-mode priority threshold to 0.
    // 设置核心优先级阈值为0（接受所有优先级>0的中断）
    *(uint32*)PLIC_SPRIORITY(hart) = 0;
}

// ask the PLIC what interrupt we should serve.
// 获取待处理的中断号
int plic_claim(void)
{
    int hart = cpuid();
    int irq  = *(uint32*)PLIC_SCLAIM(hart);
    return irq;
}

// tell the PLIC we've served this IRQ.
// 完成中断处理
void plic_complete(int irq)
{
    int hart                    = cpuid();
    *(uint32*)PLIC_SCLAIM(hart) = irq;
}
