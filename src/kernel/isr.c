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
#include <extable.h>

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

    /* 内核态异常。page fault 先查异常表——
     * 命中说明是 copy_from_user / copy_to_user 的合法 fault，
     * 跳到 fixup 恢复。 */
    if (regs->int_no == 14) {
        uint32_t fixup = extable_lookup(regs->eip);
        if (fixup) {
            regs->eip = fixup;
            return;
        }
    }

    /* 内核态异常：无法恢复 */
    panic_regs(regs, "kernel exception int=%d err=%p cr2=%p",
               (int)regs->int_no, regs->err_code, cr2);
}

/* 硬件中断处理 */
void irq_handler(struct registers *regs) {
    /* EOI 先发——让 PIC 可以接受新中断。
     * 从 PIC 的 IRQ（>= 40）额外发从 PIC EOI。 */
    if (regs->int_no >= 40) {
        __asm__ volatile("mov $0x20, %%al; out %%al, $0xA0" ::: "eax", "memory");
    }
    __asm__ volatile("mov $0x20, %%al; out %%al, $0x20" ::: "eax", "memory");

    if (regs->int_no == 32) {
        /* IRQ 0（PIT）：调度器心跳。保留硬编码——
         * 它必须在 irq_handler 里直接处理，不能走注册表。 */
        proc_reap_graveyard();
        sleep_tick();

        struct task *current = get_current_task();
        if (current) {
            current->time_slice--;
            if (current->time_slice == 0) {
                schedule();
            }
        } else {
            schedule();
        }
    } else if (regs->int_no >= 33 && regs->int_no <= 47) {
        /* IRQ 1..15：分发到注册表 */
        irq_dispatch((uint8_t)(regs->int_no - 32));
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