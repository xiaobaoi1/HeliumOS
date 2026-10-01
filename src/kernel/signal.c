#include <signal.h>
#include <task.h>
#include <scheduler.h>
#include <isr.h>
#include <tty.h>
#include <proc.h>
#include <pmm.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <uaccess.h>

/* ---------- 用户态 sigaction 镜像 ---------- */
struct user_sigaction {
    uint32_t sa_handler;
    uint32_t sa_mask;
    uint32_t sa_flags;
    uint32_t sa_restorer;
};

/* ---------- sigframe（60 字节，用户栈上） ---------- */
struct sigframe {
    uint32_t ret_addr;
    uint32_t sig;
    uint32_t saved_edi;
    uint32_t saved_esi;
    uint32_t saved_ebp;
    uint32_t saved_ebx;
    uint32_t saved_edx;
    uint32_t saved_ecx;
    uint32_t saved_eax;
    uint32_t saved_eip;
    uint32_t saved_cs;
    uint32_t saved_eflags;
    uint32_t saved_user_esp;
    uint32_t saved_user_ss;
    uint32_t saved_mask;
} __attribute__((packed));

/* ---------- 默认行为表 ---------- */
static int default_is_ignore(int sig) {
    return sig == SIGCHLD || sig == SIGCONT ||
           sig == SIGUSR1 || sig == SIGUSR2;
}

/* ---------- 前置 ---------- */
static void deliver_user_handler(struct registers *regs, struct task *cur,
                                  int sig, struct sig_action *a);

/* ---------- 异常 → 信号 ---------- */
int signal_from_exception(int int_no) {
    switch (int_no) {
        case 0:  return SIGFPE;
        case 4:  return SIGSEGV;   /* INTO overflow */
        case 6:  return SIGILL;
        case 13: return SIGSEGV;   /* GP fault */
        case 14: return SIGSEGV;   /* Page fault */
        default: return SIGSEGV;
    }
}

/* ---------- 投递 ---------- */
void signal_deliver_pending(struct registers *regs, struct task *cur) {
    if (!cur) return;
    if ((regs->cs & 3) != 3) return;   /* 只对用户态来路 */

    /* 循环：一次投递一个 handler；无 handler 的默认行为直接处理 */
    for (int iter = 0; iter < 32; iter++) {
        uint32_t pending = cur->pending_signals & ~cur->blocked_signals;
        if (pending == 0) return;

        /* 选最低位的 sig */
        int sig = 0;
        for (int i = 1; i < 32; i++) {
            if (pending & (1u << i)) { sig = i; break; }
        }
        if (sig == 0) return;

        cur->pending_signals &= ~(1u << sig);

        if (sig == SIGKILL) {
            task_terminate(cur, 128 + SIGKILL);
            proc_unref(cur);
            schedule();
            return;   /* 不返回 */
        }

        struct sig_action *a = &cur->sig_actions[sig];

        if (a->handler == SIG_IGN) continue;

        if (a->handler == SIG_DFL) {
            if (default_is_ignore(sig)) continue;
            task_terminate(cur, 128 + sig);
            proc_unref(cur);
            schedule();
            return;
        }

        /* 用户 handler */
        deliver_user_handler(regs, cur, sig, a);
        return;   /* 只投递一个 */
    }
}

/* ---------- 用户 handler 投递 ---------- */
static void deliver_user_handler(struct registers *regs, struct task *cur,
                                  int sig, struct sig_action *a) {
    uint32_t old_esp = regs->user_esp;
    uint32_t frame_size = sizeof(struct sigframe);   /* 60 */
    uint32_t new_esp = (old_esp - frame_size) & ~3u;

    if (check_user_range(new_esp, frame_size) < 0) {
        kprintf("[SIG] pid=%d handler stack overflow\n", cur->pid);
        task_terminate(cur, 128 + SIGSEGV);
        proc_unref(cur);
        schedule();
        return;
    }

    struct sigframe f = {
        .ret_addr       = a->restorer,
        .sig            = (uint32_t)sig,
        .saved_edi      = regs->edi,
        .saved_esi      = regs->esi,
        .saved_ebp      = regs->ebp,
        .saved_ebx      = regs->ebx,
        .saved_edx      = regs->edx,
        .saved_ecx      = regs->ecx,
        .saved_eax      = regs->eax,
        .saved_eip      = regs->eip,
        .saved_cs       = regs->cs,
        .saved_eflags   = regs->eflags,
        .saved_user_esp = regs->user_esp,
        .saved_user_ss  = regs->user_ss,
        .saved_mask     = cur->blocked_signals,
    };

    if (copy_to_user(new_esp, &f, sizeof(f)) < 0) {
        kprintf("[SIG] pid=%d cannot write sigframe\n", cur->pid);
        task_terminate(cur, 128 + SIGSEGV);
        proc_unref(cur);
        schedule();
        return;
    }

    regs->eip      = a->handler;
    regs->user_esp = new_esp;

    cur->blocked_signals |= a->mask | (1u << sig);
}

/* ---------- sigreturn ---------- */
void sys_sigreturn(struct registers *regs) {
    struct task *cur = get_current_task();
    if (!cur) return;

    uint32_t frame_esp = regs->user_esp;
    struct sigframe f;

    if (check_user_range(frame_esp, sizeof(f)) < 0 ||
        copy_from_user(&f, frame_esp, sizeof(f)) < 0) {
        kprintf("[SIG] pid=%d bad sigreturn frame\n", cur->pid);
        task_terminate(cur, 128 + SIGSEGV);
        proc_unref(cur);
        schedule();
        return;
    }

    /* 恢复所有寄存器 */
    regs->edi       = f.saved_edi;
    regs->esi       = f.saved_esi;
    regs->ebp       = f.saved_ebp;
    regs->ebx       = f.saved_ebx;
    regs->edx       = f.saved_edx;
    regs->ecx       = f.saved_ecx;
    regs->eax       = f.saved_eax;
    regs->eip       = f.saved_eip;
    regs->cs        = f.saved_cs;
    regs->eflags    = f.saved_eflags;
    regs->user_esp  = f.saved_user_esp;
    regs->user_ss   = f.saved_user_ss;

    cur->blocked_signals = f.saved_mask;
}

/* ---------- 系统调用：sigaction ---------- */
int sys_sigaction(int sig, void *new_user, void *old_user) {
    if (sig < 1 || sig > 31) return EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return EINVAL;

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    /* 读旧值 */
    if (old_user) {
        struct user_sigaction old = {
            .sa_handler  = cur->sig_actions[sig].handler,
            .sa_mask     = cur->sig_actions[sig].mask,
            .sa_flags    = cur->sig_actions[sig].flags,
            .sa_restorer = cur->sig_actions[sig].restorer,
        };
        if (copy_to_user((uint32_t)old_user, &old, sizeof(old)) < 0) {
            return EFAULT;
        }
    }

    /* 写新值 */
    if (new_user) {
        struct user_sigaction n;
        if (copy_from_user(&n, (uint32_t)new_user, sizeof(n)) < 0) {
            return EFAULT;
        }
        /* handler 校验：DFL/IGN 或用户空间地址 */
        if (n.sa_handler != SIG_DFL && n.sa_handler != SIG_IGN) {
            if (n.sa_handler < USER_SPACE_START) return EINVAL;
        }
        cur->sig_actions[sig].handler  = n.sa_handler;
        cur->sig_actions[sig].mask     = n.sa_mask;
        cur->sig_actions[sig].flags    = n.sa_flags;
        cur->sig_actions[sig].restorer = n.sa_restorer;
    }

    return OK;
}

/* ---------- 系统调用：sigprocmask ---------- */
int sys_sigprocmask(int how, uint32_t *set_user, uint32_t *old_user) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (old_user) {
        uint32_t old = cur->blocked_signals;
        if (copy_to_user((uint32_t)old_user, &old, sizeof(old)) < 0) {
            return EFAULT;
        }
    }

    if (!set_user) return OK;

    uint32_t set;
    if (copy_from_user(&set, (uint32_t)set_user, sizeof(set)) < 0) {
        return EFAULT;
    }

    /* SIGKILL / SIGSTOP 永远不可屏蔽 */
    set &= ~((1u << SIGKILL) | (1u << SIGSTOP));

    switch (how) {
        case SIG_BLOCK:   cur->blocked_signals |= set;  break;
        case SIG_UNBLOCK: cur->blocked_signals &= ~set; break;
        case SIG_SETMASK: cur->blocked_signals  = set;  break;
        default: return EINVAL;
    }
    return OK;
}

/* ---------- 前台信号 ---------- */
void signal_foreground(int sig) {
    struct task *fg = tty_get_foreground();
    if (!fg) return;
    if (fg->zombie) return;

    if (sig == SIGKILL) {
        task_terminate(fg, 128 + SIGKILL);
        proc_unref(fg);
        return;
    }

    fg->pending_signals |= (1u << sig);

    /* 唤醒阻塞中的前台，让它尽快处理信号 */
    if (fg->state == TASK_STATE_SLEEPING ||
        fg->state == TASK_STATE_WAITING_CHILD) {
        remove_task_from_queue(&blocked_list_head, &blocked_list_tail, fg);
        fg->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, fg);
    }
}