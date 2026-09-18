#include <syscall.h>
#include <printf.h>
#include <serial.h>
#include <task.h>
#include <stdint.h>
#include <vmm.h>
#include <pmm.h>
#include <keyboard.h>
#include <screen.h>
#include <stddef.h>
#include <errno.h>

/* 系统调用 write */
static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1) return -1;
    for (uint32_t i = 0; i < count; i++) {
        // serial_write_char(buf[i]);
        screen_write_char(buf[i]);
    }
    return count;
}

static int sys_read(int fd, char *buf, uint32_t count) {
    if (fd != 0 || !buf || count == 0) return -1;
    if ((uint32_t)buf < USER_SPACE_START ||
        (uint32_t)buf + count > USER_SPACE_END) return -1;

    int n = 0;
    while (n < count && keyboard_has_data()) {
        buf[n++] = keyboard_getchar();
    }
    return n;
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
    for (struct task *t = ready_queue_head; t; t = t->next) {
        if (t->parent == parent && (pid == -1 || t->pid == (uint32_t)pid))
            return 1;
    }
    for (struct task *t = blocked_list_head; t; t = t->next) {
        if (t->parent == parent && (pid == -1 || t->pid == (uint32_t)pid))
            return 1;
    }
    return 0;
}

/* 系统调用 waitpid */
static int sys_waitpid(int pid, int *status) {
    struct task *current = get_current_task();
    if (!current) return -1;

    while (1) {
        /* 找已退出的子进程 */
        for (struct task *t = blocked_list_head; t; t = t->next) {
            if (t->state == TASK_STATE_ZOMBIE &&
                t->parent == current &&
                (pid == -1 || t->pid == (uint32_t)pid)) {
                int child_pid = t->pid;
                if (status) *status = t->exit_status;

                remove_task_from_queue(&blocked_list_head, &blocked_list_tail, t);
                pmm_free_page((uint32_t)t);
                return child_pid;
            }
        }

        /* 没有匹配的子进程（且没有活着的），直接返回 */
        if (!has_child(current, pid)) {
            return -1;
        }

        /* 阻塞，等待子进程退出 */
        current->wait_target = NULL;   /* 等任意子进程；精确等一个的话需要找指针 */
        block_current(TASK_STATE_WAITING_CHILD);
        schedule();
    }
}

static int sys_brk(uint32_t new_brk) {
    struct task *cur = get_current_task();
    if (!cur) return -1;

    // 如果 new_brk == 0，仅返回当前 brk（查询用途）
    if (new_brk == 0) return cur->heap_brk;

    // 边界检查
    if (new_brk < cur->heap_base || new_brk > cur->heap_limit) {
        return -1;
    }

    uint32_t old_brk = cur->heap_brk;
    uint32_t start = (old_brk < new_brk) ? old_brk : new_brk;
    uint32_t end   = (old_brk < new_brk) ? new_brk : old_brk;

    // 按页对齐操作（4KB 对齐）
    uint32_t start_page = start & ~0xFFF;
    uint32_t end_page = (end + 0xFFF) & ~0xFFF;

    for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
        if (new_brk > old_brk) {
            // 扩展堆：分配物理页并映射
            uint32_t phys = pmm_alloc_page();
            if (!phys) return -1;
            vmm_map_user_page(cur->pgd, addr, phys, PTE_WRITE | PTE_USER);
        } else {
            // 收缩堆：解除映射并释放物理页
            uint32_t phys = vmm_get_phys(cur->pgd, addr);
            if (phys) {
                vmm_unmap_user_page(cur->pgd, addr);
                pmm_free_page(phys);
            }
        }
    }

    cur->heap_brk = new_brk;
    return 0; // 成功
}

// 简单复制用户空间字符串到内核缓冲区（假设用户空间地址有效）
static int strncpy_from_user(char *dest, const char *src, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len; i++) {
        // 检查 src 是否在用户空间
        if ((uint32_t)(src + i) < USER_SPACE_START ||
            (uint32_t)(src + i) > USER_SPACE_END) {
            return -1;
        }
        char c = src[i];
        dest[i] = c;
        if (c == '\0') break;
    }
    if (i == max_len) return -1; // 没有终止符
    return i + 1; // 包括终止符
}

static int sys_spawn(const char *path) {
    struct task *parent = get_current_task();
    if (!parent) return -1;

    char path_buf[128];
    if (strncpy_from_user(path_buf, path, sizeof(path_buf)) < 0) {
        return -1;
    }

    // 1. 创建子进程页目录
    uint32_t *pgd_child = vmm_create_process_page_directory();
    if (!pgd_child) return -1;

    // 2. 加载 ELF 获取入口点
    struct resolved_path rp;
    if (resolve_path(path_buf, &rp) != OK) return -1;

    struct fat32_volume *vol = (struct fat32_volume*)rp.vol->fs_private;
    if (!vol) return -1;

    uint32_t entry = load_elf_from_disk(vol, rp.path, pgd_child);
    if (!entry) {
        // 加载失败，释放页目录（简化：暂不处理）
        pmm_free_page((uint32_t)pgd_child);
        return -1;
    }

    // 3. 创建任务（复用 task_create，但 task_create 会再次分配用户栈和内核栈）
    //    注意：task_create 会调用 vmm_map_user_page 映射用户栈，还会构造 iret 帧
    //    但它不会加载 ELF，所以我们需要先加载 ELF 再调用 task_create
    //    task_create 只接受 entry_point 和 pgd，它会分配栈并设置 iret 帧
    struct task *child = task_create(entry, pgd_child);
    if (!child) {
        pmm_free_page((uint32_t)pgd_child);
        return -1;
    }

    // 4. 设置父子关系（task_create 已经设置了 parent，但为保险再设一次）
    child->parent = parent;

    // 5. 子进程已加入就绪队列（由 task_create 完成）
    return child->pid;
}

static int sys_sleep(uint32_t ms) {
    struct task *cur = get_current_task();
    if (!cur) return -1;
    if (ms == 0) return 0;

    cur->sleep_ticks = ms;
    block_current(TASK_STATE_SLEEPING);
    schedule();
    return 0;
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
        case SYS_READ:
            ret = sys_read((int)arg1, (char*)arg2, (uint32_t)arg3);
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
        case SYS_BRK:
            ret = sys_brk(arg1);
            break;
        case SYS_SPAWN:
            ret = sys_spawn((const char*)arg1);
            break;
        case SYS_SLEEP:
            ret = sys_sleep(arg1);
            break;
        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = -1;
    }

    regs->eax = ret;
}