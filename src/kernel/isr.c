#include <isr.h>
#include <syscall.h>
#include <scheduler.h>
#include <task.h>
#include <printf.h>
#include <stddef.h>
#include <keyboard.h>
#include <io.h>

extern void sleep_tick(void);


/* 异常处理 */
void isr_handler(struct registers *regs) {
    uint32_t cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    kprintf("[ISR] Exception %d (err 0x%x) at address 0x%x! EIP=0x%x, user_esp=0x%x\n",
            regs->int_no, regs->err_code, cr2, regs->eip, regs->user_esp);
    while (1) __asm__ volatile("cli; hlt");
}

/* 硬件中断处理 */
void irq_handler(struct registers *regs) {
    // 发送 EOI
    if (regs->int_no >= 40) {
        __asm__ volatile("mov $0x20, %%al; out %%al, $0xA0" ::: "eax", "memory");
    }
    __asm__ volatile("mov $0x20, %%al; out %%al, $0x20" ::: "eax", "memory");

    if (regs->int_no == 32) {
        // kprintf(".");
        sleep_tick();
        struct task *current = get_current_task();
        if (current) {
            current->time_slice--;
            if (current->time_slice == 0) {
                schedule();  // 可能触发 switch_to，不会返回
            }
        }else{
            schedule();
        }
    } else if (regs->int_no == 33) {
        keyboard_handle_irq();
    }
}

/* 系统调用处理入口 */
void isr_syscall_handler(struct registers *regs) {
    syscall_handler(regs);
}