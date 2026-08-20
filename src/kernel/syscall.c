#include <syscall.h>
#include <printf.h>
#include <serial.h>
#include <task.h>
#include <stdint.h>

/* 系统调用 write */
static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1) return -1;
    for (uint32_t i = 0; i < count; i++) {
        serial_write_char(buf[i]);
    }
    return count;
}

/* 系统调用 exit */
static void sys_exit(int status) {
    kprintf("[SYSCALL] Process %d exiting with status %d\n", get_current_task()->pid, status);
    struct task *current = get_current_task();
    if (current) {
        task_exit(current, status);
    }
    while (1) __asm__("cli; hlt");
}

/* 辅助：检查子进程是否存在（遍历所有队列） */
static int has_child(struct task *parent, int pid) {
    // 检查就绪队列
    struct task *t = ready_queue_head;
    while (t) {
        if (t->parent == parent && (pid == -1 || t->pid == (uint32_t)pid)) return 1;
        t = t->next;
    }
    // 检查僵尸队列
    t = zombie_queue_head;
    while (t) {
        if (t->parent == parent && (pid == -1 || t->pid == (uint32_t)pid)) return 1;
        t = t->next;
    }
    // 检查等待队列
    t = waiting_queue_head;
    while (t) {
        if (t->parent == parent && (pid == -1 || t->pid == (uint32_t)pid)) return 1;
        t = t->next;
    }
    // 检查当前进程（可能是运行中的子进程）
    struct task *cur = get_current_task();
    if (cur && cur->parent == parent && (pid == -1 || cur->pid == (uint32_t)pid)) {
        return 1;
    }
    return 0;
}

/* 系统调用 waitpid */
static int sys_waitpid(int pid, int *status) {
    struct task *current = get_current_task();
    if (!current) return -1;

    while (1) {
        // 查找僵尸子进程
        struct task *zombie = zombie_queue_head;
        while (zombie) {
            if (zombie->parent == current && (pid == -1 || zombie->pid == (uint32_t)pid)) {
                int child_pid = zombie->pid;
                if (status) *status = zombie->exit_status;
                remove_task_from_queue(&zombie_queue_head, &zombie_queue_tail, zombie);
                pmm_free_page((uint32_t)zombie);
                return child_pid;
            }
            zombie = zombie->next;
        }

        // 检查是否有匹配的子进程（还在运行）
        if (!has_child(current, pid)) {
            return -1;  // 没有这个子进程
        }

        // 子进程还在运行，父进程等待
        current->state = TASK_STATE_WAITING;
        remove_task_from_queue(&ready_queue_head, &ready_queue_tail, current);
        enqueue_task(&waiting_queue_head, &waiting_queue_tail, current);
        schedule();  // 让出 CPU，醒来后重新循环
    }
}

/* 系统调用分发器 */
void syscall_handler(struct registers *regs) {
    uint32_t syscall_no = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;

    int32_t ret = 0;

    switch (syscall_no) {
        case SYS_WRITE:
            ret = sys_write((int)arg1, (const char*)arg2, (uint32_t)arg3);
            break;
        case SYS_EXIT:
            sys_exit((int)arg1);
            break;
        case SYS_WAITPID:
            ret = sys_waitpid((int)arg1, (int*)arg2);
            break;
        case SYS_GETPID:
            ret = get_current_task()->pid;
            break;
        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = -1;
    }

    regs->eax = ret;
}