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
    uint32_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    int from_user = (regs->cs & 3) == 3;

    if (from_user) {
        struct task *cur = get_current_task();
        kprintf("[USER FAULT] pid=%d int=%d err=0x%x cr2=0x%x eip=0x%x\n",
                cur ? (int)cur->pid : -1,
                (int)regs->int_no, regs->err_code, cr2, regs->eip);

        if (cur) {
            /* 标记 zombie，唤醒等待者，恢复前台 */
            task_terminate(cur, 128 + (int)regs->int_no);

            /* 释放 self 引用。refcount 归零时进 graveyard */
            proc_unref(cur);
        }

        /* 让出 CPU。current 是 zombie，schedule 会跳过它 */
        schedule();

        /* schedule 不会返回（切到别的进程） */
    }

    /* 内核态异常：无法恢复 */
    kprintf("[KERNEL FAULT] int=%d err=0x%x cr2=0x%x eip=0x%x\n",
            (int)regs->int_no, regs->err_code, cr2, regs->eip);
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
        /* 先回收 graveyard（上次 task_exit 挂入的 PCB） */
        proc_reap_graveyard();

        /* tick 相关：SLEEPING 递减 + WAITING_CHILD 超时 */
        sleep_tick();

        struct task *current = get_current_task();
        if (current) {
            current->time_slice--;
            if (current->time_slice == 0) {
                schedule();
            }
        } else {
            /* current == NULL：只有启动早期（scheduler_start 之前）
             * 时钟中断先于首次调度触发时才会到这里。
             * 调 schedule 让它走 idle 兜底。 */
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