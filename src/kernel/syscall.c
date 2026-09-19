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
#include <console.h>
#include <path.h>

/* ---------- 用户指针校验辅助 ---------- */

/* 检查 [addr, addr+size) 是否完全在用户空间
 * 返回 0 成功，-1 失败
 */
static int check_user_range(uint32_t addr, uint32_t size) {
    if (size == 0) return 0;
    if (addr < USER_SPACE_START) return -1;
    uint64_t end = (uint64_t)addr + size;
    if (end > (uint64_t)USER_SPACE_END + 1) return -1;
    return 0;
}

/* 从用户空间拷贝字符串到内核缓冲区
 * 返回 >= 0 成功（拷贝的字节数含 '\0'），< 0 失败
 */
static int strncpy_from_user(char *dest, const char *src, size_t max_len) {
    if (!src || !dest || max_len == 0) return -1;
    if ((uint32_t)src < USER_SPACE_START) return -1;

    for (size_t i = 0; i < max_len; i++) {
        uint32_t addr = (uint32_t)src + i;
        if (addr < USER_SPACE_START || addr > USER_SPACE_END) {
            return -1;
        }
        char c = src[i];
        dest[i] = c;
        if (c == '\0') return (int)(i + 1);
    }
    return -1;  /* 没有终止符 */
}

/* 系统调用 write：fd=1 走控制台，其他 fd 暂不支持 */
static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1) return EINVAL;
    if (!buf || count == 0) return EINVAL;
    if ((uint32_t)buf < USER_SPACE_START ||
        (uint32_t)buf + count > USER_SPACE_END) {
        return EFAULT;
    }
    return console_write(buf, count);
}

/* 系统调用 read：fd=0 走控制台，其他 fd 暂不支持 */
static int sys_read(int fd, char *buf, uint32_t count) {
    if (fd != 0) return EINVAL;
    if (!buf || count == 0) return EINVAL;
    if ((uint32_t)buf < USER_SPACE_START ||
        (uint32_t)buf + count > USER_SPACE_END) {
        return EFAULT;
    }
    return console_read(buf, count);
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
    if (!current) return EINVAL;

    /* status 可以为 NULL（表示不关心退出码） */
    if (status != NULL) {
        if (check_user_range((uint32_t)status, sizeof(int)) < 0) {
            return EFAULT;
        }
    }

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
                kprintf("[WAITPID] Freed PCB pid=%d at phys %p\n", child_pid, (uint32_t)t);
                return child_pid;
            }
        }

        /* 没有匹配的子进程（且没有活着的），直接返回 */
        if (!has_child(current, pid)) {
            return ECHILD;
        }

        /* 阻塞，等待子进程退出 */
        current->wait_target = NULL;   /* 等任意子进程；精确等一个的话需要找指针 */
        block_current(TASK_STATE_WAITING_CHILD);
        schedule();
    }
}

static int sys_brk(uint32_t new_brk) {
    struct task *cur = get_current_task();
    kprintf("[BRK] pid=%d new=0x%x heap_brk=0x%x\n",
            cur ? (int)cur->pid : -1,
            new_brk,
            cur ? cur->heap_brk : 0);
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
            // 扩展堆：分配物理页并映射
            uint32_t phys = pmm_alloc_page();
            if (!phys) return ENOMEM;
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

static int sys_spawn(const char *path) {
    struct task *parent = get_current_task();
    if (!parent) return EINVAL;                /* 原来是 EFAULT */

    char path_buf[PATH_MAX_LEN];
    if (strncpy_from_user(path_buf, path, sizeof(path_buf)) < 0) {
        return EFAULT;                          /* 保持 */
    }

    /* 1. 创建子进程页目录 */
    uint32_t *pgd_child = vmm_create_process_page_directory();
    if (!pgd_child) return ENOMEM;              /* 原来是 -1 */

    /* 2. 加载 ELF 获取入口点 */
    struct resolved_path rp;
    int rr = resolve_path(path_buf, &rp);
    if (rr != OK) {
        vmm_free_process_address_space(pgd_child);
        return rr;                              /* 传回 ENOENT / EINVAL */
    }

    struct fat32_volume *vol = (struct fat32_volume*)rp.vol->fs_private;
    if (!vol) {
        vmm_free_process_address_space(pgd_child);
        return ENOENT;                          /* 原来是 -1 */
    }

    uint32_t entry = load_elf_from_disk(vol, rp.path, pgd_child);
    if (!entry) {
        vmm_free_process_address_space(pgd_child);
        return ENOEXEC;                         /* 原来是 -1 */
    }

    /* 3. 创建任务 */
    struct task *child = task_create(entry, pgd_child);
    if (!child) {
        vmm_free_process_address_space(pgd_child);
        return ENOMEM;                          /* 原来是 -1 */
    }

    child->parent = parent;
    return child->pid;
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
        case SYS_CONSOLE_CLEAR:
            console_clear();
            ret = 0;
            break;

        case SYS_CONSOLE_SET_COLOR:
            console_set_color((uint8_t)arg1, (uint8_t)arg2);
            ret = 0;
            break;

        case SYS_CONSOLE_SET_CURSOR:
            console_set_cursor((int)arg1, (int)arg2);
            ret = 0;
            break;

        case SYS_CONSOLE_GET_CURSOR:
            /* arg1 指向用户空间的 int[2]: out_x, out_y */
            if ((uint32_t)arg1 < USER_SPACE_START ||
                (uint32_t)arg1 + 8 > USER_SPACE_END) {
                ret = EFAULT;
            } else {
                int cx = 0, cy = 0;
                console_get_cursor(&cx, &cy);
                ((int*)arg1)[0] = cx;
                ((int*)arg1)[1] = cy;
                ret = 0;
            }
            break;

        case SYS_CONSOLE_SAVE_CURSOR:
            console_save_cursor();
            ret = 0;
            break;

        case SYS_CONSOLE_RESTORE_CURSOR:
            console_restore_cursor();
            ret = 0;
            break;

        case SYS_CONSOLE_DEBUG_WRITE:
            if (!arg1 || arg2 == 0) {
                ret = EINVAL;
            } else if ((uint32_t)arg1 < USER_SPACE_START ||
                       (uint32_t)arg1 + arg2 > USER_SPACE_END) {
                ret = EFAULT;
            } else {
                ret = console_debug_write((const char*)arg1, arg2);
            }
            break;



        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = ENOSYS;
    }

    regs->eax = ret;
}