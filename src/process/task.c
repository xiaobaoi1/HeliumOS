#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>
#include <isr.h>
#include <errno.h>
#include <tty.h>
#include <vma.h>

extern void enter_user_mode(void);

static uint32_t next_pid = 1;

/* 分配一个未被占用的 PID。回绕时跳过 0。 */
static uint32_t alloc_pid(void) {
    for (uint32_t tries = 0; tries < 0xFFFFFFFFu; tries++) {
        uint32_t pid = next_pid++;
        if (next_pid == 0) next_pid = 1;   /* 跳过 0 */
        if (!proc_find_by_pid(pid)) return pid;
    }
    return 0;   /* 全部占满——理论不可达 */
}

struct task *current_task = NULL;

/* 两个队列 */
struct task *ready_queue_head = NULL;
struct task *ready_queue_tail = NULL;
struct task *blocked_list_head = NULL;
struct task *blocked_list_tail = NULL;

/* ---------- 队列操作 ---------- */
void enqueue_task(struct task **head, struct task **tail, struct task *task) {
    task->next = NULL;
    if (*tail) {
        (*tail)->next = task;
        *tail = task;
    } else {
        *head = *tail = task;
    }
}

struct task *dequeue_task(struct task **head, struct task **tail) {
    if (!*head) return NULL;
    struct task *task = *head;
    *head = task->next;
    if (!*head) *tail = NULL;
    task->next = NULL;
    return task;
}

void remove_task_from_queue(struct task **head, struct task **tail, struct task *task) {
    if (!*head) return;
    if (*head == task) {
        *head = task->next;
        if (!*head) *tail = NULL;
        task->next = NULL;
        return;
    }
    struct task *prev = *head;
    while (prev->next && prev->next != task) {
        prev = prev->next;
    }
    if (prev->next == task) {
        prev->next = task->next;
        if (prev->next == NULL) *tail = prev;
        task->next = NULL;
    }
}

/* 把内核缓冲写入子进程用户空间（支持跨页） */
static int write_to_child_stack(uint32_t *pgd, uint32_t u_virt,
                                const void *buf, uint32_t len) {
    const uint8_t *src = (const uint8_t*)buf;
    while (len > 0) {
        uint32_t phys = vmm_get_phys(pgd, u_virt & ~0xFFF);
        if (!phys) return -1;
        uint32_t off = u_virt & 0xFFF;
        uint32_t n = 0x1000 - off;
        if (n > len) n = len;
        memcpy((void*)(phys + off), src, n);
        u_virt += n;
        src += n;
        len -= n;
    }
    return 0;
}

/* 写 4 字节到子进程用户空间（4 字节对齐） */
static void write_u32_to_child(uint32_t *pgd, uint32_t u_virt, uint32_t val) {
    if ((u_virt & 0xFFF) > 0xFFC) {
        write_to_child_stack(pgd, u_virt, &val, 4);
        return;
    }
    uint32_t phys = vmm_get_phys(pgd, u_virt & ~0xFFF);
    if (!phys) return;
    *(uint32_t*)(phys + (u_virt & 0xFFF)) = val;
}

/* ---------- 阻塞与唤醒 ---------- */
void block_current(uint32_t new_state) {
    struct task *cur = get_current_task();
    if (!cur) return;

    /* 从就绪队列移除（可能在也可能不在） */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, cur);

    cur->state = new_state;
    enqueue_task(&blocked_list_head, &blocked_list_tail, cur);
}

void unblock_task(struct task *t, uint32_t new_state) {
    if (!t) return;

    /* 从 blocked_list 移除 */
    remove_task_from_queue(&blocked_list_head, &blocked_list_tail, t);

    t->state = new_state;
    enqueue_task(&ready_queue_head, &ready_queue_tail, t);
}

/* 唤醒所有等待 target 退出的进程。
 * wait_target == NULL 表示"等任意子进程退出"（waitpid(-1) 风格），
 * 具体指针表示"等特定子进程"。
 * 两种情况在同一个字段里区分——NULL 是通配，非 NULL 是精确匹配。 */
void wake_up_waiters(struct task *target) {
    if (!target) return;
    for (struct task *t = blocked_list_head; t; ) {
        struct task *next = t->next;
        /* wait_target == NULL 是通配（等任意子进程），
         * 非 NULL 是精确匹配——两者共用同一字段。 */
        if (t->state == TASK_STATE_WAITING_CHILD &&
            (t->wait_target == NULL || t->wait_target == target)) {
            t->wait_target = NULL;
            unblock_task(t, TASK_STATE_READY);
        }
        t = next;
    }
}

/* 处理 slot 继承或重定向。
 *
 *   child_slot    子进程的 slot
 *   parent_slot   父进程对应的 slot（NULL 表示无父进程）
 *   child         子进程（用于 fs_dup_handle）
 *   parent        父进程（NULL 表示内核早期创建）
 *   redir_fd      重定向参数：SPAWN_FD_INHERIT / SPAWN_FD_NULL / >= 0
 *
 * 宽容策略：任何失败都 fallback 到 IO_SLOT_DEFAULT，不返回错误。
 */
static void setup_slot(struct io_slot *child_slot,
                       const struct io_slot *parent_slot,
                       struct task *child,
                       struct task *parent,
                       int redir_fd,
                       int redir_pipe) {
    /* 0. pipe 优先 */
    if (redir_pipe >= 0 && parent) {
        int new_h, new_type;
        if (ipc_dup_handle(parent, redir_pipe, child,
                           &new_h, &new_type) == 0) {
            if (new_type == IPC_PIPE_READ) {
                child_slot->type = IO_SLOT_PIPE_READ;
            } else {
                child_slot->type = IO_SLOT_PIPE_WRITE;
            }
            child_slot->fd = new_h;
            return;
        }
        /* 复制失败 → fallback */
    }

    /* 1. NULL */
    if (redir_fd == SPAWN_FD_NULL) {
        child_slot->type = IO_SLOT_NULL;
        child_slot->fd = -1;
        return;
    }
    /* 2. TTY */
    if (redir_fd == SPAWN_FD_TTY) {
        child_slot->type = IO_SLOT_DEFAULT;
        child_slot->fd = -1;
        return;
    }

    if (!parent) return;

    /* 3. 显式 fd */
    if (redir_fd >= 0) {
        if (redir_fd >= FS_MAX_HANDLES) return;
        if (!parent->fs_handles[redir_fd].used) return;
        fd_t new_fd = fs_dup_handle(parent, redir_fd, child);
        if (new_fd >= 0) {
            child_slot->type = IO_SLOT_FILE;
            child_slot->fd = new_fd;
        }
        return;
    }

    /* 4. INHERIT */
    if (!parent_slot) return;
    child_slot->type = parent_slot->type;
    child_slot->fd = parent_slot->fd;
    if (parent_slot->type == IO_SLOT_FILE) {
        if (parent_slot->fd < 0 || parent_slot->fd >= FS_MAX_HANDLES) {
            child_slot->type = IO_SLOT_DEFAULT;
            child_slot->fd = -1;
            return;
        }
        fd_t new_fd = fs_dup_handle(parent, parent_slot->fd, child);
        if (new_fd >= 0) child_slot->fd = new_fd;
        else { child_slot->type = IO_SLOT_DEFAULT; child_slot->fd = -1; }
    }
    /* PIPE 类型暂不继承（in_pipe 显式传） */
    if (parent_slot->type == IO_SLOT_PIPE_READ ||
        parent_slot->type == IO_SLOT_PIPE_WRITE) {
        child_slot->type = IO_SLOT_DEFAULT;
        child_slot->fd = -1;
    }
}

/* ---------- 进程创建 ---------- */
struct task *task_create(uint32_t entry_point, uint32_t *pgd,
                         int argc, char *const argv[],
                         int envc, char *const envp[],
                         int redir_in_fd, int redir_out_fd, int redir_err_fd,
                         int redir_in_pipe, int redir_out_pipe, int redir_err_pipe) {
    /* 1. 分配所有物理资源，任何一步失败都回滚 */
    struct task *task = (struct task*)pmm_alloc_page();
    if (!task) {
        kprintf("[TASK] ERROR: Failed to allocate PCB.\n");
        return NULL;
    }

    uint32_t kernel_stack = pmm_alloc_page();
    if (!kernel_stack) {
        pmm_free_page((uint32_t)task);
        return NULL;
    }

    uint32_t stack_low = pmm_alloc_page();
    if (!stack_low) {
        pmm_free_page(kernel_stack);
        pmm_free_page((uint32_t)task);
        return NULL;
    }

    uint32_t stack_high = pmm_alloc_page();
    if (!stack_high) {
        pmm_free_page(stack_low);
        pmm_free_page(kernel_stack);
        pmm_free_page((uint32_t)task);
        return NULL;
    }

    /* 2. 初始化字段 */
    uint32_t new_pid = alloc_pid();
    if (new_pid == 0) {
        /* 理论上不可达。此处释放已分配资源，返回失败。 */
        pmm_free_page(stack_high);
        pmm_free_page(stack_low);
        pmm_free_page(kernel_stack);
        pmm_free_page((uint32_t)task);
        return NULL;
    }
    task->pid = new_pid;
    task->state = TASK_STATE_READY;
    task->time_slice = TIME_SLICE_TICKS;
    task->pgd = pgd;
    task->entry_point = entry_point;
    task->next = NULL;
    task->wait_target = NULL;
    task->sleep_ticks = 0;

    task->creator_pid = get_current_task() ? get_current_task()->pid : 0;
    task->wait_deadline = 0;

    task->refcount = 1;
    task->zombie = 0;
    task->proc_next = NULL;
    task->grave_next = NULL;
    for (int i = 0; i < PROC_MAX_HANDLES; i++) {
        task->proc_handles[i].used = 0;
        task->proc_handles[i].access = 0;
        task->proc_handles[i].task = NULL;
    }

    /* 初始化三个 slot */
    task->stdin_slot.type  = IO_SLOT_DEFAULT;
    task->stdin_slot.fd    = -1;
    task->stdout_slot.type = IO_SLOT_DEFAULT;
    task->stdout_slot.fd   = -1;
    task->stderr_slot.type = IO_SLOT_DEFAULT;
    task->stderr_slot.fd   = -1;

    /* 初始化信号 */
    task->pending_signals = 0;
    task->blocked_signals = 0;
    for (int i = 0; i < 32; i++) {
        task->sig_actions[i].handler  = SIG_DFL;
        task->sig_actions[i].mask     = 0;
        task->sig_actions[i].flags    = 0;
        task->sig_actions[i].restorer = 0;
    }

    /* 初始化 IPC */
    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        task->ipc_handles[i].used = 0;
        task->ipc_handles[i].pipe = NULL;
    }
    task->wait_pipe = NULL;

    /* 初始化 VMA */
    task->vma_list = NULL;


    task->kernel_stack_phys = kernel_stack;
    task->user_stack_virt = 0x7FFFE000;

    /* 3. 映射用户栈 */
    vmm_map_user_page(pgd, 0x7FFFC000, stack_low, PTE_WRITE | PTE_USER);
    vmm_map_user_page(pgd, 0x7FFFD000, stack_high, PTE_WRITE | PTE_USER);

    /* 3b. 建栈 VMA。覆盖两页 [0x7FFFC000, 0x7FFFE000)。 */
    if (vma_insert(task, 0x7FFFC000, 0x7FFFE000,
                   VMA_READ | VMA_WRITE | VMA_USER,
                   VMA_TYPE_STACK, NULL, 0) != OK) {
        KLOG_WARN("VMA: stack insert failed for pid %d\n", task->pid);
    }

    /* 4. 加入全局链表 */
    proc_register(task);

    /* 5. 堆 */
    task->heap_base = 0x50000000;
    task->heap_brk = 0x50000000;
    task->heap_limit = 0x60000000;

    /* 6. cwd */
    struct task *parent = get_current_task();
    if (parent && parent->cwd_volume[0] != '\0') {
        strcpy(task->cwd_volume, parent->cwd_volume);
        strcpy(task->cwd_path, parent->cwd_path);
    } else {
        struct volume *def = volume_get_default();
        if (def) {
            strcpy(task->cwd_volume, def->name);
        } else {
            task->cwd_volume[0] = '\0';
        }
        strcpy(task->cwd_path, "/");
    }

    /* 7. 清空 fs/dev 句柄表 */
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        task->fs_handles[i].used = 0;
    }
    for (int i = 0; i < DEV_MAX_HANDLES; i++) {
        task->dev_handles[i].used = 0;
    }

    /* 8. 在用户栈上构造 argc/argv/envp
     *
     * 栈布局（从高地址到低地址）：
     *   环境变量字符串
     *   参数字符串
     *   [4 字节对齐]
     *   envp[0..envc-1], NULL
     *   argv[0..argc-1], NULL
     *   argc
     *   ← user_sp（新的 esp）
     */
    uint32_t user_sp = task->user_stack_virt;
    uint32_t arg_addrs[ARGV_MAX];
    uint32_t env_addrs[ENVP_MAX];
    int valid_argc = 0;
    int valid_envc = 0;

    /* ----- 环境变量字符串（最高地址） ----- */
    if (envc > ENVP_MAX) envc = ENVP_MAX;
    if (envc > 0 && envp) {
        for (int i = 0; i < envc; i++) {
            if (!envp[i]) break;
            uint32_t len = strlen(envp[i]) + 1;
            if (len > ENV_MAX) len = ENV_MAX;
            user_sp -= len;
            if (write_to_child_stack(pgd, user_sp, envp[i], len) < 0) {
                user_sp += len;
                break;
            }
            env_addrs[valid_envc++] = user_sp;
        }
    }

    /* ----- 参数字符串 ----- */
    if (argc > ARGV_MAX) argc = ARGV_MAX;
    if (argc > 0 && argv) {
        for (int i = 0; i < argc; i++) {
            if (!argv[i]) break;
            uint32_t len = strlen(argv[i]) + 1;
            if (len > ARG_MAX) len = ARG_MAX;
            user_sp -= len;
            if (write_to_child_stack(pgd, user_sp, argv[i], len) < 0) {
                user_sp += len;
                break;
            }
            arg_addrs[i] = user_sp;
            valid_argc++;
        }
    }

    /* 4 字节对齐 */
    user_sp &= ~3u;

    /* ----- envp 数组 + NULL ----- */
    user_sp -= (valid_envc + 1) * 4;
    uint32_t envp_arr = user_sp;
    for (int i = 0; i < valid_envc; i++) {
        write_u32_to_child(pgd, envp_arr + i * 4, env_addrs[i]);
    }
    write_u32_to_child(pgd, envp_arr + valid_envc * 4, 0);

    /* ----- argv 数组 + NULL ----- */
    user_sp -= (valid_argc + 1) * 4;
    uint32_t argv_arr = user_sp;
    for (int i = 0; i < valid_argc; i++) {
        write_u32_to_child(pgd, argv_arr + i * 4, arg_addrs[i]);
    }
    write_u32_to_child(pgd, argv_arr + valid_argc * 4, 0);

    /* ----- argc ----- */
    user_sp -= 4;
    write_u32_to_child(pgd, user_sp, (uint32_t)valid_argc);

    /* 9. 处理三个 slot 的重定向
     * 从父进程复制 fs_handle 到子进程（如果指定了 fd）。
     * 父进程不存在（内核早期创建 idle/shell）或 fd 无效时，
     * slot 保持初始化时的 IO_SLOT_DEFAULT。 */

    // struct task *parent = get_current_task();

    setup_slot(&task->stdin_slot,
               parent ? &parent->stdin_slot : NULL,
               task, parent, redir_in_fd, redir_in_pipe);
    setup_slot(&task->stdout_slot,
               parent ? &parent->stdout_slot : NULL,
               task, parent, redir_out_fd, redir_out_pipe);
    setup_slot(&task->stderr_slot,
               parent ? &parent->stderr_slot : NULL,
               task, parent, redir_err_fd, redir_err_pipe);


    /* 10. 构造内核栈（iret 帧的 user esp 指向 argc 位置） */
    uint32_t *sp = (uint32_t*)(task->kernel_stack_phys + 4096);

    /* iret 帧 */
    *(--sp) = 0x23;                    /* ss */
    *(--sp) = user_sp;                 /* user esp ← 指向 argc */
    *(--sp) = 0x202;                   /* eflags */
    *(--sp) = 0x1B;                    /* cs */
    *(--sp) = task->entry_point;       /* eip */

    /* 中断占位 */
    *(--sp) = 0;
    *(--sp) = 0;

    /* pusha */
    *(--sp) = 0;                       /* eax */
    *(--sp) = 0;                       /* ecx */
    *(--sp) = 0;                       /* edx */
    *(--sp) = 0;                       /* ebx */
    *(--sp) = 0;                       /* esp */
    *(--sp) = 0;                       /* ebp */
    *(--sp) = 0;                       /* esi */
    *(--sp) = 0;                       /* edi */

    /* 段寄存器 */
    *(--sp) = 0x23;
    *(--sp) = 0x23;
    *(--sp) = 0x23;
    *(--sp) = 0x23;

    /* 返回地址（trampoline） */
    *(--sp) = (uint32_t)enter_user_mode;

    /* callee-saved */
    *(--sp) = 0;                       /* ebp */
    *(--sp) = 0;                       /* ebx */
    *(--sp) = 0;                       /* esi */
    *(--sp) = 0;                       /* edi */

    task->kernel_esp = (uint32_t)sp;

    enqueue_task(&ready_queue_head, &ready_queue_tail, task);

    kprintf("[TASK] Process %d created. Entry: %p, Kernel stack: %p, heap_brk=%p\n",
        task->pid, entry_point, task->kernel_stack_phys, task->heap_brk);

    return task;
}

struct task *get_current_task(void) { return current_task; }

void set_current_task(struct task *task) {
    current_task = task;
    if (task) task->state = TASK_STATE_RUNNING;
}

void scheduler_start(void) {
    struct task *first = dequeue_task(&ready_queue_head, &ready_queue_tail);
    if (!first) {
        while (1) __asm__("cli; hlt");
    }
    set_current_task(first);

    /* switch_to 会切 CR3、更新 TSS.esp0、加载 first->kernel_esp，
     * 通过 ret 跳到 enter_user_mode，最终 iret 进入用户态。
     * 本函数不会返回。 */
    switch_to(NULL, first);

    while (1) __asm__("cli; hlt");
}

/* ---------- 强制终止 ---------- */
void task_terminate(struct task *t, int status) {
    if (!t) return;
    if (t->zombie) return;

    kprintf("[TASK] Process %d is terminated with status %d\n", t->pid, status);

    t->exit_status = status;
    t->zombie = 1;
    t->state = TASK_STATE_ZOMBIE;

    /* 释放持有的所有句柄——和 task_exit 一致。
     * 不放这里会泄漏：pipe 计数不减、proc_handles 里的子进程 refcount 不减。 */
    fs_release_all(t);
    dev_release_all(t);
    ipc_release_all(t);
    proc_release_all_handles(t);

    /* 从任何队列移除 */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, t);
    remove_task_from_queue(&blocked_list_head, &blocked_list_tail, t);

    /* 加入 blocked_list 作为僵尸 */
    enqueue_task(&blocked_list_head, &blocked_list_tail, t);

    /* 唤醒所有等它的 */
    wake_up_waiters(t);

    /* 如果它是键盘前台，恢复父进程为前台 */
    if (tty_get_foreground() == t) {
        struct task *parent = NULL;
        if (t->creator_pid != 0) {
            parent = proc_find_by_pid(t->creator_pid);
        }
        tty_set_foreground(parent);
    }
}

/* task_exit: 进程退出。
 *
 * 不释放 kernel_stack / pgd / PCB——这些由 proc_free_pcb 释放。
 * proc_unref 归零且 t == current 时把 task 挂入 graveyard，
 * 由 irq_handler 开头的 proc_reap_graveyard 回收。
 *
 * 依赖：
 *   - schedule 能识别 ZOMBIE 状态的 current（不放回 ready_queue）
 *   - switch_to 支持 prev != NULL 的正常保存（不需要 prev == NULL 特例）
 */
/* ---------- 进程退出 ---------- */
void task_exit(struct task *task, int status) {
    if (!task) return;

    kprintf("[TASK] Process %d exiting with status %d\n", task->pid, status);
    task->exit_status = status;

    /* 释放自己持有的所有句柄（对子进程的引用） */
    fs_release_all(task);
    dev_release_all(task);
    ipc_release_all(task);
    proc_release_all_handles(task);

    /* 从就绪队列移除 */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, task);

    /* 标记 zombie，挂入 blocked_list */
    task->zombie = 1;
    task->state = TASK_STATE_ZOMBIE;
    enqueue_task(&blocked_list_head, &blocked_list_tail, task);

    /* 唤醒所有等它的进程 */
    wake_up_waiters(task);

    /* 如果自己是键盘前台，恢复父进程为前台 */
    if (tty_get_foreground() == task) {
        struct task *parent = NULL;
        if (task->creator_pid != 0) {
            parent = proc_find_by_pid(task->creator_pid);
        }
        tty_set_foreground(parent);   /* 可能为 NULL */
    }
    
    /* 释放 self 引用。如果 refcount 归零且 t == current，
     * proc_unref 会把 task 挂入 graveyard，由 irq_handler 回收。 */
    proc_unref(task);

    /* schedule 里 current = task，state 是 ZOMBIE，
     * 不满足"时间片耗尽"的分支，直接被跳过。
     * switch_to 会保存 task->kernel_esp 但不会再恢复。 */
    schedule();
}