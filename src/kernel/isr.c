#include <isr.h>
#include <syscall.h>
#include <scheduler.h>
#include <task.h>
#include <printf.h>
#include <stddef.h>
#include <keyboard.h>
#include <io.h>
#include <signal.h>
#include <panic.h>

extern void sleep_tick(void);


/* 异常处理 */
void isr_handler(struct registers *regs) {
    uint32_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    int from_user = (regs->cs & 3) == 3;

    if (from_user) {
        struct task *cur = get_current_task();
        int sig = signal_from_exception(regs->int_no);

        kprintf("[USER FAULT] pid=%d int=%d sig=%d cr2=0x%x eip=0x%x\n",
                cur ? (int)cur->pid : -1,
                (int)regs->int_no, sig, cr2, regs->eip);

        if (cur) {
            struct sig_action *a = &cur->sig_actions[sig];

            if (a->handler == SIG_DFL || a->handler == SIG_IGN) {
                /* 无 handler（或忽略）：直接终止 */
                task_terminate(cur, 128 + sig);
                proc_unref(cur);
                schedule();
                return;   /* 不返回 */
            }

            /* 有 handler：投递 */
            cur->pending_signals |= (1u << sig);
            signal_deliver_pending(regs, cur);
            return;
        }
    }

    /* 内核态异常：无法恢复 */
    panic_regs(regs, "kernel exception int=%d err=%p cr2=%p",
               (int)regs->int_no, regs->err_code, cr2);
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
    struct task *cur = get_current_task();
    if (cur) {
        signal_deliver_pending(regs, cur);
    }
}

/* 系统调用处理入口 */
void isr_syscall_handler(struct registers *regs) {
    syscall_handler(regs);
}