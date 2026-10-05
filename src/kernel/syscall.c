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
#include <signal.h>
#include <ipc.h>
#include <uaccess.h>
#include <rtc.h>
#include <acpi.h>
#include <shm.h>
#include <vma.h>
#include <net_sock.h>

#define PROT_READ   0x01
#define PROT_WRITE  0x02
#define PROT_EXEC   0x04

#define MAP_PRIVATE    0x01
#define MAP_SHARED     0x02
#define MAP_ANONYMOUS  0x04
#define MAP_FIXED      0x10

#define MMAP_BASE  0x60000000u
#define MMAP_END   0x70000000u

#define PROC_LIST_MAX 64

/* 用户态结构体镜像。字段顺序和 user/libc/include/syscall.h 保持一致。
 * size 用于版本兼容：内核只读 min(size, 本结构体大小) 字节。 */
struct spawn_params {
    uint32_t size;
    int32_t  in_fd;
    int32_t  out_fd;
    int32_t  err_fd;
    int32_t  in_pipe;
    int32_t  out_pipe;
    int32_t  err_pipe;
    uint32_t flags;
    uint32_t envp;
};

#define SPAWN_FD_INHERIT   (-1)   /* 不重定向 */
#define SPAWN_FD_NULL      (-2)   /* 丢弃 */

static int sys_write(int fd, const char *buf, uint32_t count) {
    if (fd != 1 && fd != 2) return EINVAL;
    if (!buf || count == 0) return EINVAL;
    if (check_user_range((uint32_t)buf, count) < 0) return EFAULT;

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    struct io_slot *slot = (fd == 1) ? &cur->stdout_slot : &cur->stderr_slot;

    switch (slot->type) {
        case IO_SLOT_DEFAULT:
            return tty_write(buf, count);
        case IO_SLOT_FILE: {
            int h = slot->fd;
            if (h < 0 || h >= FS_MAX_HANDLES) return EINVAL;
            if (!cur->fs_handles[h].used) return EINVAL;
            return fs_write(h, buf, count);
        }
        case IO_SLOT_PIPE_WRITE: {
            int h = slot->fd;
            if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
            if (!cur->ipc_handles[h].used) return EINVAL;
            return pipe_write(cur->ipc_handles[h].pipe,
                              (const uint8_t*)buf, count);
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
        
        case IO_SLOT_PIPE_READ: {
            int h = cur->stdin_slot.fd;
            if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
            if (!cur->ipc_handles[h].used) return EINVAL;
            return pipe_read(cur->ipc_handles[h].pipe,
                             (uint8_t*)buf, count);
        }


        case IO_SLOT_NULL:
            return 0;    /* EOF */
        default:
            return ENOSYS;
    }
}

/* 系统调用 exit */
static void sys_exit(int status) {
    KLOG_DBG("[SYSCALL] Process %d exiting with status %d\n", get_current_task()->pid, status);
    struct task *current = get_current_task();
    if (current) {
        task_exit(current, status);
    }
    while (1) __asm__("cli; hlt");
}

/* 通用等待逻辑。target == NULL 表示等任意子进程 */
static int wait_impl(struct task *cur, struct task *target,
                     int *status, uint32_t timeout_ms) {
    uint64_t deadline = 0;
    if (timeout_ms > 0) deadline = get_ticks() + timeout_ms;

    while (1) {
        /* 被信号打断 */
        if (cur->pending_signals & ~cur->blocked_signals) {
            return EINTR;
        }
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

/* ---------- proc_list ---------- */

static int sys_proc_list(uint32_t *user_buf, int max) {
    struct task *cur = get_current_task();
    if (!cur || !user_buf || max <= 0) return EINVAL;
    if (max > PROC_LIST_MAX) max = PROC_LIST_MAX;
    if (check_user_range((uint32_t)user_buf, (uint32_t)max * 4) < 0)
        return EFAULT;

    /* syscall 上下文 IF=0（int 0x80 中断门自动关中断），
     * proc_list_head 不会被时钟中断修改。直接遍历安全。 */
    uint32_t tmp[PROC_LIST_MAX];
    int n = proc_list(tmp, max);
    if (copy_to_user((uint32_t)user_buf, tmp, (uint32_t)n * 4) < 0)
        return EFAULT;
    return n;
}

/* ---------- uname ---------- */

struct uname_buf {
    char sysname[16];
    char release[16];
    char machine[16];
};

static int sys_uname(struct uname_buf *user_buf) {
    if (!user_buf) return EINVAL;
    if (check_user_range((uint32_t)user_buf, sizeof(struct uname_buf)) < 0)
        return EFAULT;

    struct uname_buf u;
    memset(&u, 0, sizeof(u));
    strncpy(u.sysname, "HeliumOS", sizeof(u.sysname) - 1);
    strncpy(u.release, "0.1",     sizeof(u.release) - 1);
    strncpy(u.machine, "i686",    sizeof(u.machine) - 1);

    if (copy_to_user((uint32_t)user_buf, &u, sizeof(u)) < 0)
        return EFAULT;
    return OK;
}

/* ---------- fstat ---------- */

static int sys_fstat(int fd, struct fstat_buf *user_buf) {
    if (!user_buf) return EINVAL;
    if (check_user_range((uint32_t)user_buf, sizeof(struct fstat_buf)) < 0)
        return EFAULT;

    struct fstat_buf tmp;
    int r = fs_fstat(fd, &tmp);
    if (r != OK) return r;
    if (copy_to_user((uint32_t)user_buf, &tmp, sizeof(tmp)) < 0)
        return EFAULT;
    return OK;
}

static int sys_brk(uint32_t new_brk) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (new_brk == 0) return cur->heap_brk;
    if (new_brk < cur->heap_base || new_brk > cur->heap_limit) return EINVAL;

    uint32_t old_brk = cur->heap_brk;
    if (new_brk == old_brk) return 0;

    if (new_brk < old_brk) {
        /* 收缩：unmap 完整落在 [new_brk, old_brk) 内的页并释放物理页。
         * 扩展：不分配——page fault 时按需分配。 */
        for (uint32_t page = (new_brk + 0xFFF) & ~0xFFF;
             page + 0x1000 <= old_brk;
             page += 0x1000) {
            uint32_t phys = vmm_get_phys(cur->pgd, page);
            if (phys) {
                vmm_unmap_user_page(cur->pgd, page);
                pmm_free_page(phys & ~0xFFF);
            }
        }
    }

    cur->heap_brk = new_brk;
    return 0;
}

/* 在 MMAP_BASE..MMAP_END 找 len 字节的空闲区间 */
static uint32_t mmap_find_free(struct task *t, uint32_t len) {
    uint32_t addr = MMAP_BASE;
    while (addr + len <= MMAP_END) {
        int free_here = 1;
        for (struct vma *v = t->vma_list; v; v = v->next) {
            if (v->start < addr + len && v->end > addr) {
                free_here = 0;
                addr = v->end;
                break;
            }
        }
        if (free_here) return addr;
    }
    return 0;
}

static uint32_t sys_mmap(uint32_t addr, uint32_t length, uint32_t prot,
                         uint32_t flags, int shm_idx) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (length == 0) return EINVAL;
    if (length > 0x10000000) return EINVAL;

    length = (length + 0xFFF) & ~0xFFFu;

    int is_shared = (flags & MAP_SHARED) != 0;
    int is_anon   = (flags & MAP_ANONYMOUS) != 0;

    if (is_shared && is_anon) return EINVAL;   /* 不支持匿名的命名共享 */
    if (is_shared && shm_idx < 0) return EINVAL;

    uint32_t start;
    if (addr != 0) {
        if (addr & 0xFFF) return EINVAL;
        if (addr < USER_SPACE_START || addr + length > USER_SPACE_END)
            return EINVAL;
        start = addr;
    } else {
        start = mmap_find_free(cur, length);
        if (start == 0) return ENOMEM;
    }

    uint32_t vma_flags = VMA_USER;
    if (prot & PROT_READ)  vma_flags |= VMA_READ;
    if (prot & PROT_WRITE) vma_flags |= VMA_WRITE;
    if (prot & PROT_EXEC)  vma_flags |= VMA_EXEC;

    struct shm *s = NULL;
    if (is_shared) {
        s = shm_at(shm_idx);
        if (!s) return EINVAL;
        if (length > s->num_pages * 0x1000) return EINVAL;
    }

    int r = vma_insert(cur, start, start + length, vma_flags,
                       VMA_TYPE_MMAP, s, 0);
    if (r != OK) return (uint32_t)r;

    return start;
}

static int sys_munmap(uint32_t addr, uint32_t length) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (addr == 0 || (addr & 0xFFF)) return EINVAL;
    if (length == 0) return EINVAL;

    length = (length + 0xFFF) & ~0xFFFu;
    uint32_t end = addr + length;

    /* 先 unmap 物理页（已映射的） */
    for (uint32_t page = addr; page < end; page += 0x1000) {
        uint32_t phys = vmm_get_phys(cur->pgd, page);
        if (phys) {
            struct vma *v = vma_find(cur, page);
            /* 共享页由 shm 对象管理，不释放；只解映射 */
            int is_shared = (v && v->shm);
            vmm_unmap_user_page(cur->pgd, page);
            if (!is_shared) pmm_free_page(phys & ~0xFFF);
        }
    }

    vma_remove_range(cur, addr, end);
    return OK;
}

static int sys_spawn(const char *path, char *const argv[], void *params_user) {
    struct task *parent = get_current_task();
    if (!parent) return EINVAL;

    char path_buf[PATH_MAX_LEN];
    if (strncpy_from_user(path_buf, path, sizeof(path_buf)) < 0)
        return EFAULT;

    /* 解析 params */
    int redir_in = SPAWN_FD_INHERIT;
    int redir_out = SPAWN_FD_INHERIT;
    int redir_err = SPAWN_FD_INHERIT;
    int redir_in_pipe = -1;
    int redir_out_pipe = -1;
    int redir_err_pipe = -1;
    uint32_t envp_user = 0;

    if (params_user) {
        struct spawn_params p = {0};
        if (check_user_range((uint32_t)params_user, sizeof(uint32_t)) < 0)
            return EFAULT;
        uint32_t user_size;
        if (copy_from_user(&user_size, (uint32_t)params_user, sizeof(uint32_t)) < 0)
            return EFAULT;
        if (user_size < sizeof(uint32_t)) return EINVAL;
        uint32_t to_copy = user_size < sizeof(p) ? user_size : sizeof(p);
        if (copy_from_user(&p, (uint32_t)params_user, to_copy) < 0)
            return EFAULT;
        redir_in  = p.in_fd;
        redir_out = p.out_fd;
        redir_err = p.err_fd;
        redir_in_pipe  = p.in_pipe;
        redir_out_pipe = p.out_pipe;
        redir_err_pipe = p.err_pipe;
        envp_user = p.envp;
    }

    /* 拷贝 argv */
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

    /* 拷贝 envp */
    char *k_envp[ENVP_MAX];
    int envc = 0;

    if (envp_user) {
        for (int i = 0; i < ENVP_MAX; i++) {
            uint32_t entry_addr = envp_user + i * 4;
            if (check_user_range(entry_addr, sizeof(char*)) < 0) break;
            char *u_env;
            if (copy_from_user(&u_env, entry_addr, sizeof(u_env)) < 0) break;
            if (!u_env) break;
            char *k_env = kmalloc(ENV_MAX);
            if (!k_env) break;
            if (strncpy_from_user(k_env, u_env, ENV_MAX) < 0) {
                kfree(k_env);
                break;
            }
            k_envp[envc++] = k_env;
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
                                     envc, k_envp,
                                     redir_in, redir_out, redir_err,
                                     redir_in_pipe, redir_out_pipe, redir_err_pipe);
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
    for (int i = 0; i < envc; i++) kfree(k_envp[i]);
    return h;

cleanup:
    for (int i = 0; i < argc; i++) kfree(k_argv[i]);
    for (int i = 0; i < envc; i++) kfree(k_envp[i]);
    return ret;
}

/* kill(handle, sig)
 *   handle == 0 表示发给自己
 *   sig == SIGKILL：立即终止（不允许自杀）
 *   其他 sig：进入 pending，唤醒阻塞中的目标 */
static int sys_kill(proc_handle_t h, int sig) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (sig < 1 || sig > 31) return EINVAL;

    struct task *target;
    if (h == -1) {
        target = cur;    /* 发给自己 */
    } else {
        target = proc_handle_deref(h, PROC_TERMINATE);
        if (!target) return EINVAL;
    }

    if (target->zombie) return EINVAL;

    if (sig == SIGKILL) {
        if (target == cur) return EPERM;   /* 自杀用 exit */
        task_terminate(target, 128 + SIGKILL);
        proc_unref(target);
        return OK;
    }

    /* 其他：进 pending */
    target->pending_signals |= (1u << sig);

    /* 唤醒阻塞中的目标——让它尽快返回 EINTR 或处理信号 */
    if (target->state == TASK_STATE_SLEEPING ||
        target->state == TASK_STATE_WAITING_CHILD) {
        remove_task_from_queue(&blocked_list_head, &blocked_list_tail, target);
        target->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, target);
    }
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

/* 打开一个进程，获得 handle。access 是 PROC_* 掩码。 */
static int sys_proc_open(uint32_t pid, uint8_t access) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (access == 0 || (access & ~PROC_ALL)) return EINVAL;

    struct task *target = proc_find_by_pid(pid);
    if (!target) return ENOENT;
    if (target->zombie) return ENOENT;

    return proc_handle_alloc(target, access);
}

static int sys_sleep(uint32_t ms) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (ms == 0) return 0;

    cur->sleep_ticks = ms;
    block_current(TASK_STATE_SLEEPING);
    schedule();

    /* 被信号打断 */
    if (cur->pending_signals & ~cur->blocked_signals) {
        return EINTR;
    }
    return 0;
}

static int sys_tty_set_foreground(proc_handle_t h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (h == -1) {
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
    uint32_t arg4 = regs->esi;
    uint32_t arg5 = regs->edi;

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
        case SYS_GETPPID:
            ret = (int)get_current_task()->creator_pid;
            break;
        case SYS_PROC_LIST:
            ret = sys_proc_list((uint32_t*)arg1, (int)arg2);
            break;
        case SYS_BRK:
            ret = sys_brk(arg1);
            break;
        case SYS_MMAP:
            ret = (int)sys_mmap(arg1, arg2, arg3, arg4, (int)arg5);
            break;
        case SYS_MUNMAP:
            ret = sys_munmap(arg1, arg2);
            break;
        case SYS_SHM_OPEN:
            ret = sys_shm_open((const char*)arg1, arg2, (int)arg3);
            break;
        case SYS_SHM_UNLINK:
            ret = sys_shm_unlink((const char*)arg1);
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
        case SYS_PROC_OPEN:
            ret = sys_proc_open((uint32_t)arg1, (uint8_t)arg2);
            break;
        
        case SYS_SIGACTION:
            ret = sys_sigaction((int)arg1, (void*)arg2, (void*)arg3);
            break;
        case SYS_SIGPROCMASK:
            ret = sys_sigprocmask((int)arg1, (uint32_t*)arg2, (uint32_t*)arg3);
            break;
        case SYS_SIGRETURN:
            sys_sigreturn(regs);
            return;   /* 跳过 regs->eax = ret */

        

        case SYS_PIPE:
            ret = sys_pipe((int*)arg1);
            break;
        case SYS_PIPE_READ:
            ret = sys_pipe_read((int)arg1, (void*)arg2, (uint32_t)arg3);
            break;
        case SYS_PIPE_WRITE:
            ret = sys_pipe_write((int)arg1, (const void*)arg2, (uint32_t)arg3);
            break;
        case SYS_IPC_CLOSE:
            ret = sys_ipc_close((int)arg1);
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
        case SYS_FS_FSTAT:
            ret = sys_fstat(arg1, (struct fstat_buf*)arg2);
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
        case SYS_FS_UNLINK: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0)
                ret = EFAULT;
            else
                ret = fs_unlink(path_buf);
            break;
        }
        case SYS_FS_MKDIR: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0)
                ret = EFAULT;
            else
                ret = fs_mkdir(path_buf);
            break;
        }
        case SYS_FS_RMDIR: {
            char path_buf[PATH_MAX_LEN];
            if (strncpy_from_user(path_buf, (const char*)arg1, sizeof(path_buf)) < 0)
                ret = EFAULT;
            else
                ret = fs_rmdir(path_buf);
            break;
        }
        case SYS_FS_RENAME: {
            char old_buf[PATH_MAX_LEN], new_buf[PATH_MAX_LEN];
            if (strncpy_from_user(old_buf, (const char*)arg1, sizeof(old_buf)) < 0 ||
                strncpy_from_user(new_buf, (const char*)arg2, sizeof(new_buf)) < 0)
                ret = EFAULT;
            else
                ret = fs_rename(old_buf, new_buf);
            break;
        }
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
        case SYS_UNAME:
            ret = sys_uname((struct uname_buf*)arg1);
            break;

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

        

        /* ---------- 系统控制 ---------- */
        case SYS_RTC_GET_TIME: {
            if (check_user_range(arg1, sizeof(struct rtc_time)) < 0) {
                ret = EFAULT;
            } else {
                struct rtc_time t;
                int r = rtc_read(&t);
                if (r < 0) {
                    ret = r;
                } else if (copy_to_user(arg1, &t, sizeof(t)) < 0) {
                    ret = EFAULT;
                } else {
                    ret = OK;
                }
            }
            break;
        }
        case SYS_REBOOT:
            if (arg1 == ACPI_CMD_HALT) {
                ret = acpi_poweroff();
            } else if (arg1 == ACPI_CMD_REBOOT) {
                ret = acpi_reboot();
            } else {
                ret = EINVAL;
            }
            /* 成功的话系统已经关了/重启了；到这里说明失败 */
            break;
        
        
            /* ---------- 网络 ---------- */
        case SYS_SOCKET:
            ret = sys_socket((int)arg1, (int)arg2, (int)arg3);
            break;
        case SYS_BIND:
            ret = sys_bind((int)arg1, (const void*)arg2, arg3);
            break;
        case SYS_SENDTO:
            ret = sys_sendto((int)arg1, (const void*)arg2, arg3,
                             (const void*)arg4, arg5);
            break;
        case SYS_RECVFROM:
            ret = sys_recvfrom((int)arg1, (void*)arg2, arg3,
                               (void*)arg4, (uint32_t*)arg5);
            break;
        case SYS_SOCKCLOSE:
            ret = sys_sockclose((int)arg1);
            break;
        case SYS_CONNECT:
            ret = sys_connect((int)arg1, (const void*)arg2, arg3);
            break;
        case SYS_LISTEN:
            ret = sys_listen((int)arg1, (int)arg2);
            break;
        case SYS_ACCEPT:
            ret = sys_accept((int)arg1, (void*)arg2, (uint32_t*)arg3);
            break;

        default:
            kprintf("[SYSCALL] Unknown syscall %d\n", syscall_no);
            ret = ENOSYS;
    }

    // kprintf("[SYSCALL] regs=%p regs->eax=%d ret=%d\n",
    //         regs, regs->eax, ret);


    regs->eax = ret;

    /* 返回用户态前投递 pending signals。
     * 可能改 regs->eip / user_esp（投递 handler）
     * 或杀进程 + schedule（不返回）。 */
    struct task *cur = get_current_task();
    if (cur) {
        signal_deliver_pending(regs, cur);
    }
}