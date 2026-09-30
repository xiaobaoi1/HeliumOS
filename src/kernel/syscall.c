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
#include <fs.h>
#include <device.h>
#include <tty.h>
#include <path.h>
#include <proc.h>


/* 用户态结构体镜像。字段顺序和 user/libc/include/syscall.h 保持一致。
 * size 用于版本兼容：内核只读 min(size, 本结构体大小) 字节。 */
struct spawn_redirect {
    uint32_t size;
    int32_t  in_fd;
    int32_t  out_fd;
    int32_t  err_fd;
    uint32_t flags;
};

#define SPAWN_FD_INHERIT   (-1)   /* 不重定向 */
#define SPAWN_FD_NULL      (-2)   /* 丢弃 */

/* ---------- 用户指针校验辅助 ---------- */

/* 检查用户地址范围 [addr, addr+size) 是否完全在用户空间且已映射
 * 返回 0 成功，-1 失败
 */
static int check_user_range(uint32_t addr, uint32_t size) {
    if (size == 0) return 0;
    if (addr < USER_SPACE_START) return -1;
    uint64_t end = (uint64_t)addr + size;
    if (end > (uint64_t)USER_SPACE_END + 1) return -1;

    /* 逐页检查映射。前提：调用者是 syscall 分发器，current_task 一定存在。 */
    struct task *cur = get_current_task();
    if (!cur || !cur->pgd) return -1;

    uint32_t first_page = addr & ~0xFFF;
    uint32_t last_page  = ((uint32_t)(end - 1)) & ~0xFFF;

    for (uint32_t p = first_page; p <= last_page; p += 4096) {
        if (!vmm_get_phys(cur->pgd, p)) return -1;
    }
    return 0;
}

/* 从用户空间拷贝字符串到内核缓冲区
 * 返回 >= 0 成功（拷贝的字节数含 '\0'），< 0 失败
 */
static int strncpy_from_user(char *dest, const char *src, size_t max_len) {
    if (!src || !dest || max_len == 0) return -1;
    if ((uint32_t)src < USER_SPACE_START) return -1;

    struct task *cur = get_current_task();
    if (!cur || !cur->pgd) return -1;

    uint32_t u_src = (uint32_t)src;
    uint32_t cur_page = u_src & ~0xFFF;
    if (!vmm_get_phys(cur->pgd, cur_page)) return -1;

    for (size_t i = 0; i < max_len; i++) {
        uint32_t addr = u_src + i;
        if (addr < USER_SPACE_START || addr > USER_SPACE_END) {
            return -1;
        }

        /* 跨页时重新检查映射 */
        uint32_t page = addr & ~0xFFF;
        if (page != cur_page) {
            if (!vmm_get_phys(cur->pgd, page)) return -1;
            cur_page = page;
        }

        char c = src[i];
        dest[i] = c;
        if (c == '\0') return (int)(i + 1);
    }
    return -1;
}

/* 从用户空间拷贝 n 字节到内核缓冲。
 * 用户指针须已通过 check_user_range。 */
static int memcpy_from_user(void *dest, const void *src, size_t n) {
    if (n == 0) return 0;
    if (!dest || !src) return -1;
    if (check_user_range((uint32_t)src, n) < 0) return -1;
    memcpy(dest, src, n);
    return 0;
}

static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1) return EINVAL;
    if (!buf || count == 0) return EINVAL;
    if (check_user_range((uint32_t)buf, count) < 0) return EFAULT;

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    
    switch (cur->stdout_slot.type) {
        case IO_SLOT_DEFAULT:
            return tty_write(buf, count);

        case IO_SLOT_FILE: {
            int h = cur->stdout_slot.fd;
            if (h < 0 || h >= FS_MAX_HANDLES) return EINVAL;
            if (!cur->fs_handles[h].used) return EINVAL;
            return fs_write(h, buf, count);
        }

        case IO_SLOT_NULL:
            return (int)count;

        default:
            return ENOSYS;
    }
}

static int sys_read(int fd, char *buf, uint32_t count) {
    if (fd != 0) return EINVAL;
    if (!buf || count == 0) return EINVAL;
    if (check_user_range((uint32_t)buf, count) < 0) return EFAULT;

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    switch (cur->stdin_slot.type) {
        case IO_SLOT_DEFAULT:
            return tty_read(buf, count);

        case IO_SLOT_FILE: {
            int h = cur->stdin_slot.fd;
            if (h < 0 || h >= FS_MAX_HANDLES) return EINVAL;
            if (!cur->fs_handles[h].used) return EINVAL;
            return fs_read(h, buf, count);
        }

        case IO_SLOT_NULL:
            return 0;    /* EOF */

        default:
            return ENOSYS;
    }
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

/* 通用等待逻辑。target == NULL 表示等任意子进程 */
static int wait_impl(struct task *cur, struct task *target,
                     int *status, uint32_t timeout_ms) {
    uint32_t deadline = 0;
    if (timeout_ms > 0) deadline = get_ticks() + timeout_ms;

    while (1) {
        /* 1. 目标已退出 */
        if (target) {
            if (target->zombie) {
                if (status) *status = target->exit_status;
                return OK;
            }
        } else {
            /* 等任意子进程：找第一个 zombie */
            for (int i = 0; i < PROC_MAX_HANDLES; i++) {
                if (!cur->proc_handles[i].used) continue;
                if (!(cur->proc_handles[i].access & PROC_WAIT)) continue;
                struct task *t = cur->proc_handles[i].task;
                if (t->zombie) {
                    if (status) *status = t->exit_status;
                    return OK;
                }
            }
            /* 检查是否还有活着的子进程 */
            int has_alive = 0;
            for (int i = 0; i < PROC_MAX_HANDLES; i++) {
                if (!cur->proc_handles[i].used) continue;
                if (!(cur->proc_handles[i].access & PROC_WAIT)) continue;
                if (!cur->proc_handles[i].task->zombie) {
                    has_alive = 1;
                    break;
                }
            }
            if (!has_alive) return ECHILD;
        }

        /* 2. 超时 */
        if (deadline > 0 && get_ticks() >= deadline) {
            return EAGAIN;
        }

        /* 3. 阻塞
         * schedule 会切走；被唤醒时从 switch_to 的 ret 返回，
         * 继续循环；最终 return 会流回 sys_wait → syscall_handler，
         * 由 regs->eax = ret 写到用户态。 */
        cur->wait_target   = target;
        cur->wait_deadline = deadline;
        block_current(TASK_STATE_WAITING_CHILD);
        schedule();
    }
}

static int sys_wait(proc_handle_t h, int *status, uint32_t timeout_ms) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (status) {
        if (check_user_range((uint32_t)status, sizeof(int)) < 0) return EFAULT;
    }

    struct task *target = proc_handle_deref(h, PROC_WAIT);
    if (!target) return EINVAL;

    return wait_impl(cur, target, status, timeout_ms);
}

/* 注意：本函数靠"句柄只能由 spawn 分配给当前进程"这一不变式
 * 来保证 pid 一定是当前进程的子进程。将来引入句柄继承后，
 * 需要显式检查 target->creator_pid == cur->pid。 */
/* 系统调用 waitpid */
static int sys_waitpid(int pid, int *status) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (status) {
        if (check_user_range((uint32_t)status, sizeof(int)) < 0) return EFAULT;
    }

    if (pid == -1) {
        /* 等任意子进程 */
        return wait_impl(cur, NULL, status, 0);
    }

    /* 找匹配 pid 的 handle */
    for (int i = 0; i < PROC_MAX_HANDLES; i++) {
        if (!cur->proc_handles[i].used) continue;
        if (!(cur->proc_handles[i].access & PROC_WAIT)) continue;
        if (cur->proc_handles[i].task->pid == (uint32_t)pid) {
            return wait_impl(cur, cur->proc_handles[i].task, status, 0);
        }
    }
    return ECHILD;
}

static int sys_brk(uint32_t new_brk) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    // 如果 new_brk == 0，仅返回当前 brk（查询用途）
    if (new_brk == 0) return cur->heap_brk;

    // 边界检查
    if (new_brk < cur->heap_base || new_brk > cur->heap_limit) {
        return EINVAL;
    }

    uint32_t old_brk = cur->heap_brk;
    uint32_t start = (old_brk < new_brk) ? old_brk : new_brk;
    uint32_t end   = (old_brk < new_brk) ? new_brk : old_brk;

    // 按页对齐操作（4KB 对齐）
    uint32_t start_page = start & ~0xFFF;
    uint32_t end_page = (end + 0xFFF) & ~0xFFF;

    for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
            if (new_brk > old_brk) {
            /* 扩展：逐页分配映射；失败时回滚已映射的部分 */
            uint32_t next_unmapped = start_page;
            for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
                uint32_t phys = pmm_alloc_page();
                if (!phys) {
                    /* 回滚：释放本次已映射的页 */
                    for (uint32_t a = start_page; a < next_unmapped; a += 4096) {
                        uint32_t p = vmm_get_phys(cur->pgd, a);
                        if (p) {
                            vmm_unmap_user_page(cur->pgd, a);
                            pmm_free_page(p);
                        }
                    }
                    return ENOMEM;
                }
                vmm_map_user_page(cur->pgd, addr, phys, PTE_WRITE | PTE_USER);
                next_unmapped = addr + 4096;
            }
        } else {
            /* 收缩：逐页解映射并释放 */
            for (uint32_t addr = start_page; addr < end_page; addr += 4096) {
                uint32_t phys = vmm_get_phys(cur->pgd, addr);
                if (phys) {
                    vmm_unmap_user_page(cur->pgd, addr);
                    pmm_free_page(phys);
                }
            }
        }

        cur->heap_brk = new_brk;
        return 0;
    }

    cur->heap_brk = new_brk;
    return 0; // 成功
}

static int sys_spawn(const char *path, char *const argv[], void *redir_user) {
    struct task *parent = get_current_task();
    if (!parent) return EINVAL;

    char path_buf[PATH_MAX_LEN];
    if (strncpy_from_user(path_buf, path, sizeof(path_buf)) < 0)
        return EFAULT;

    /* 解析重定向 */
    int redir_in = SPAWN_FD_INHERIT;
    int redir_out = SPAWN_FD_INHERIT;
    int redir_err = SPAWN_FD_INHERIT;

    if (redir_user) {
        struct spawn_redirect r = {0};
        if (check_user_range((uint32_t)redir_user, sizeof(uint32_t)) < 0)
            return EFAULT;
        /* 先读 size */
        uint32_t user_size;
        if (memcpy_from_user(&user_size, redir_user, sizeof(uint32_t)) < 0)
            return EFAULT;
        if (user_size < sizeof(uint32_t)) return EINVAL;
        uint32_t to_copy = user_size < sizeof(r) ? user_size : sizeof(r);
        if (memcpy_from_user(&r, redir_user, to_copy) < 0)
            return EFAULT;
        redir_in = r.in_fd;
        redir_out = r.out_fd;
        redir_err = r.err_fd;
    }

    /* argv 拷贝（同前） */
    char *k_argv[ARGV_MAX];
    int argc = 0;
    int ret = 0;

    if (argv) {
        for (int i = 0; i < ARGV_MAX; i++) {
            if (check_user_range((uint32_t)(argv + i), sizeof(char*)) < 0) {
                ret = EFAULT;
                goto cleanup;
            }
            char *u_arg = argv[i];
            if (!u_arg) break;
            char *k_arg = kmalloc(ARG_MAX);
            if (!k_arg) { ret = ENOMEM; goto cleanup; }
            if (strncpy_from_user(k_arg, u_arg, ARG_MAX) < 0) {
                kfree(k_arg);
                ret = EFAULT;
                goto cleanup;
            }
            k_argv[argc++] = k_arg;
        }
    }

    uint32_t *pgd_child = vmm_create_process_page_directory();
    if (!pgd_child) { ret = ENOMEM; goto cleanup; }

    struct resolved_path rp;
    int rr = resolve_path(path_buf, &rp);
    if (rr != OK) {
        vmm_free_process_address_space(pgd_child);
        ret = rr;
        goto cleanup;
    }

    struct fat32_volume *vol = (struct fat32_volume*)rp.vol->fs_private;
    if (!vol) {
        vmm_free_process_address_space(pgd_child);
        ret = ENOENT;
        goto cleanup;
    }

    uint32_t entry = load_elf_from_disk(vol, rp.path, pgd_child);
    if (!entry) {
        vmm_free_process_address_space(pgd_child);
        ret = ENOEXEC;
        goto cleanup;
    }

    struct task *child = task_create(entry, pgd_child, argc, k_argv,
                                     redir_in, redir_out, redir_err);
    if (!child) {
        vmm_free_process_address_space(pgd_child);
        ret = ENOMEM;
        goto cleanup;
    }

    proc_handle_t h = proc_handle_alloc(child, PROC_ALL);
    if (h < 0) {
        remove_task_from_queue(&ready_queue_head, &ready_queue_tail, child);
        child->zombie = 1;
        proc_unref(child);
        ret = ENOMEM;
        goto cleanup;
    }

    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    return h;

cleanup:
    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    return ret;
}

/* 注意：本函数的"允许 kill 非子进程"这一行为，
 * 当前依赖"句柄只能由 spawn 分配给子进程"来约束。
 * 将来引入句柄继承/传递后，需要额外的权限检查。 */
static int sys_kill(proc_handle_t h, int status) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    struct task *target = proc_handle_deref(h, PROC_TERMINATE);
    if (!target) return EINVAL;

    if (target == cur) return EPERM;    /* 不允许 kill 自己 */
    if (target->zombie) return EINVAL;  /* 已经退出 */

    /* 标记退出 */
    target->exit_status = status;
    target->zombie = 1;
    target->state = TASK_STATE_ZOMBIE;

    /* 从任何队列移除 */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, target);
    remove_task_from_queue(&blocked_list_head, &blocked_list_tail, target);

    /* 加入 blocked_list */
    enqueue_task(&blocked_list_head, &blocked_list_tail, target);

    /* 唤醒所有等它的 */
    wake_up_waiters(target);

    /* 如果 target 是前台，恢复父进程 */
    if (tty_get_foreground() == target) {
        struct task *parent = NULL;
        if (target->creator_pid != 0) {
            parent = proc_find_by_pid(target->creator_pid);
        }
        tty_set_foreground(parent);
    }

    /* 释放 self 引用：被 kill 的进程不会再走 task_exit */
    proc_unref(target);

    return OK;
}

static int sys_process_close(proc_handle_t h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PROC_MAX_HANDLES) return EINVAL;
    if (!cur->proc_handles[h].used) return EINVAL;

    struct task *target = cur->proc_handles[h].task;
    cur->proc_handles[h].used = 0;
    cur->proc_handles[h].access = 0;
    cur->proc_handles[h].task = NULL;

    proc_unref(target);
    return OK;
}

static int sys_sleep(uint32_t ms) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (ms == 0) return 0;

    cur->sleep_ticks = ms;
    block_current(TASK_STATE_SLEEPING);
    schedule();
    return 0;
}

static int sys_tty_set_foreground(proc_handle_t h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (h == 0) {
        /* 显式清空（自己拿回键盘）*/
        tty_set_foreground(cur);
        return OK;
    }

    struct task *target = proc_handle_deref(h, PROC_QUERY);
    if (!target) return EINVAL;

    tty_set_foreground(target);
    return OK;
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
        case SYS_GETPID:
            ret = get_current_task()->pid;
            break;
        case SYS_BRK:
            ret = sys_brk(arg1);
            break;
        case SYS_SPAWN:
            ret = sys_spawn((const char*)arg1, (char *const*)arg2, (void*)arg3);
            break;
        case SYS_SLEEP:
            ret = sys_sleep(arg1);
            break;
        case SYS_WAIT:
            ret = sys_wait((proc_handle_t)arg1, (int*)arg2, arg3);
            break;
        case SYS_KILL:
            ret = sys_kill((proc_handle_t)arg1, (int)arg2);
            break;
        case SYS_PROC_CLOSE:
            ret = sys_process_close((proc_handle_t)arg1);
            break;
        


                /* ---------- 文件系统 ---------- */
        case SYS_FS_OPEN: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_open(path_buf, arg2);
            }
            break;
        }
        case SYS_FS_READ:
            if (check_user_range(arg2, arg3) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_read(arg1, (void*)arg2, arg3);
            }
            break;
        case SYS_FS_WRITE:
            if (check_user_range(arg2, arg3) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_write(arg1, (const void*)arg2, arg3);
            }
            break;
        case SYS_FS_SEEK:
            ret = fs_seek(arg1, arg2);
            break;
        case SYS_FS_CLOSE:
            ret = fs_close(arg1);
            break;
        case SYS_FS_OPENDIR: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_opendir(path_buf);
            }
            break;
        }
        case SYS_FS_READDIR:
            if (check_user_range(arg2, sizeof(struct dirent)) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_readdir(arg1, (struct dirent*)arg2);
            }
            break;
        case SYS_FS_CLOSEDIR:
            ret = fs_closedir(arg1);
            break;
        case SYS_GETCWD:
            if (check_user_range(arg1, arg2) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_getcwd((char*)arg1, arg2);
            }
            break;
        case SYS_CHDIR: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0) {
                ret = EFAULT;
            } else {
                ret = fs_chdir(path_buf);
            }
            break;
        }

        /* ---------- 设备 ---------- */
        case SYS_DEV_OPEN:
            /* arg2 语义由设备类型决定，暂不校验 */
            ret = dev_open(arg1, (void*)arg2);
            break;
        case SYS_DEV_READ:
            if (check_user_range(arg2, arg3) < 0) {
                ret = EFAULT;
            } else {
                ret = dev_read(arg1, (void*)arg2, arg3);
            }
            break;
        case SYS_DEV_WRITE:
            if (check_user_range(arg2, arg3) < 0) {
                ret = EFAULT;
            } else {
                ret = dev_write(arg1, (const void*)arg2, arg3);
            }
            break;
        case SYS_DEV_IOCTL:
            /* arg3 语义由 cmd 决定，暂不校验 */
            ret = dev_ioctl(arg1, arg2, (void*)arg3);
            break;
        case SYS_DEV_CLOSE:
            ret = dev_close(arg1);
            break;


        /* ---------- 控制台 ---------- */
        case SYS_TTY_CLEAR:
            tty_clear();
            ret = 0;
            break;

        case SYS_TTY_SET_COLOR:
            if (arg1 > 15 || arg2 > 15) {
                ret = EINVAL;
            } else {
                tty_set_color((uint8_t)arg1, (uint8_t)arg2);
                ret = 0;
            }
            break;

        case SYS_TTY_SET_CURSOR:
            tty_set_cursor((int)arg1, (int)arg2);
            ret = 0;
            break;

        case SYS_TTY_GET_CURSOR:
            /* arg1 指向用户空间的 int[2]: out_x, out_y */
            if (check_user_range(arg1, 8) < 0) {
                ret = EFAULT;
            } else {
                int cx = 0, cy = 0;
                tty_get_cursor(&cx, &cy);
                ((int*)arg1)[0] = cx;
                ((int*)arg1)[1] = cy;
                ret = 0;
            }
            break;

        case SYS_TTY_SAVE_CURSOR:
            tty_save_cursor();
            ret = 0;
            break;

        case SYS_TTY_RESTORE_CURSOR:
            tty_restore_cursor();
            ret = 0;
            break;

        case SYS_TTY_DEBUG_WRITE:
            if (!arg1 || arg2 == 0) {
                ret = EINVAL;
            } else if (check_user_range(arg1, arg2) < 0) {
                ret = EFAULT;
            } else {
                ret = tty_debug_write((const char*)arg1, arg2);
            }
            break;
        case SYS_TTY_SET_FOREGROUND:
            ret = sys_tty_set_foreground((proc_handle_t)arg1);
            break;



        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = ENOSYS;
    }

    // kprintf("[SYSCALL] regs=%p regs->eax=%d ret=%d\n",
    //         regs, regs->eax, ret);


    regs->eax = ret;
}