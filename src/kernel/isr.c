#include <isr.h>
#include <syscall.h>
#include <printf.h>
#include <stdint.h>

/* 异常处理 */
void isr_handler(struct registers *regs) {
    kprintf("[ISR] Exception %d (err 0x%x) occured! Halting.\n", regs->int_no, regs->err_code);
    while (1) __asm__ volatile("cli; hlt");
}

/* 硬件中断处理 */
void irq_handler(struct registers *regs) {
    if (regs->int_no >= 40) {
        __asm__ volatile("mov $0x20, %%al; out %%al, $0xA0" ::: "eax", "memory");
    }
    __asm__ volatile("mov $0x20, %%al; out %%al, $0x20" ::: "eax", "memory");

    if (regs->int_no == 32) {
        // 时钟中断
    }
}

/* 系统调用处理入口 */
void isr_syscall_handler(struct registers *regs) {
    syscall_handler(regs);
}