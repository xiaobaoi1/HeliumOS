#include <printf.h>
#include <stdint.h>

/* 寄存器结构（由汇编推送的顺序） */
struct registers {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
};

/* 异常处理（CPU异常） */
void isr_handler(struct registers *regs) {
    kprintf("[ISR] Exception %d (err 0x%x) occured! Halting.\n", regs->int_no, regs->err_code);
    while (1) __asm__ volatile("cli; hlt");
}

/* 硬件中断处理（IRQ） */
void irq_handler(struct registers *regs) {
    /* 发送EOI（结束中断） */
    if (regs->int_no >= 40) {
        // 从属PIC
        __asm__ volatile("mov $0x20, %%al; out %%al, $0xA0" ::: "eax", "memory");
    }
    // 主PIC
    __asm__ volatile("mov $0x20, %%al; out %%al, $0x20" ::: "eax", "memory");

    /* 暂时只打印信息，以后可扩展 */
    // kprintf("[IRQ] IRQ %d handled.\n", regs->int_no - 32);

    /* 特别处理IRQ0（时钟）和IRQ1（键盘） */
    if (regs->int_no == 32) {
        // 时钟中断：可以在这里调用调度器
    }
}