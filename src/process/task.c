#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>
#include <isr.h>
#include <errno.h>

extern void enter_user_mode(void);

static uint32_t next_pid = 1;
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

void wake_up_waiters(struct task *target) {
    if (!target) return;
    for (struct task *t = blocked_list_head; t; ) {
        struct task *next = t->next;
        if (t->state == TASK_STATE_WAITING_CHILD &&
            (t->wait_target == NULL || t->wait_target == target)) {
            t->wait_target = NULL;
            unblock_task(t, TASK_STATE_READY);
        }
        t = next;
    }
}

/* ---------- 进程创建 ---------- */
struct task *task_create(uint32_t entry_point, uint32_t *pgd) {
    struct task *task = (struct task*)pmm_alloc_page();
    if (!task) {
        kprintf("[TASK] ERROR: Failed to allocate PCB.\n");
        return NULL;
    }

    task->pid = next_pid++;
    task->state = TASK_STATE_READY;
    task->time_slice = TIME_SLICE_TICKS;
    task->pgd = pgd;
    task->entry_point = entry_point;
    task->next = NULL;
    task->wait_target = NULL;
    task->sleep_ticks = 0;


    task->creator_pid = get_current_task() ? get_current_task()->pid : 0;
    task->wait_deadline = 0;

    /* 进程对象字段 */
    task->refcount = 1;              /* self 引用 */
    task->zombie = 0;
    task->proc_next = NULL;
    task->grave_next = NULL;
    for (int i = 0; i < PROC_MAX_HANDLES; i++) {
        task->proc_handles[i].used = 0;
        task->proc_handles[i].access = 0;
        task->proc_handles[i].task = NULL;
    }
    
    // 内核栈
    task->kernel_stack_phys = pmm_alloc_page();
    if (!task->kernel_stack_phys) {
        kprintf("[TASK] ERROR: Failed to allocate kernel stack.\n");
        return NULL;
    }

    uint32_t stack_low = pmm_alloc_page();
    uint32_t stack_high = pmm_alloc_page();
    if (!stack_low || !stack_high) {
        kprintf("[TASK] ERROR: Failed to allocate user stack pages.\n");
        return NULL;
    }


    /* 加入全局链表 */
    proc_register(task);

    // 用户栈
    task->user_stack_virt = 0x7FFFE000;
    task->user_stack_phys = stack_high;

    vmm_map_user_page(pgd, 0x7FFFC000, stack_low, PTE_WRITE | PTE_USER);
    vmm_map_user_page(pgd, 0x7FFFD000, stack_high, PTE_WRITE | PTE_USER);


    // 堆
    task->heap_base = 0x50000000;
    task->heap_brk = 0x50000000;
    task->heap_limit = 0x60000000;


    /* 初始化 cwd */
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

    /* 清空 fs 句柄表 */
    for (int i = 0; i < FS_MAX_HANDLES; i++) {
        task->fs_handles[i].used = 0;
    }

    for (int i = 0; i < DEV_MAX_HANDLES; i++) {
        task->dev_handles[i].used = 0;
    }

        /* 构造内核栈：模拟"刚从 switch_to 返回"的状态。
     *
     * 栈布局（从低地址到高地址）：
     *   [callee-saved: edi, esi, ebx, ebp]      ← kernel_esp 指向这里
     *   [返回地址 = enter_user_mode]
     *   [段寄存器: gs, fs, es, ds]
     *   [pusha: edi, esi, ebp, (esp), ebx, edx, ecx, eax]
     *   [int_no, err_code]
     *   [iret 帧: eip, cs, eflags, user_esp, ss]
     *
     * 执行过程：
     *   switch_to(NULL, task) 的 pop 弹出 callee-saved；
     *   ret 跳到 enter_user_mode；
     *   enter_user_mode 恢复段寄存器 + pusha，跳过 int/err，iret 到用户态。
     */
    uint32_t *sp = (uint32_t*)(task->kernel_stack_phys + 4096);

    /* iret 帧（5 个 dword，从高到低写） */
    *(--sp) = 0x23;                    /* ss */
    *(--sp) = task->user_stack_virt;   /* user esp */
    *(--sp) = 0x202;                   /* eflags: IF=1 */
    *(--sp) = 0x1B;                    /* cs */
    *(--sp) = task->entry_point;       /* eip */

    /* 中断占位（2 个 dword） */
    *(--sp) = 0;                       /* err_code */
    *(--sp) = 0;                       /* int_no */

    /* pusha 保存（8 个 dword，从高到低） */
    *(--sp) = 0;                       /* eax */
    *(--sp) = 0;                       /* ecx */
    *(--sp) = 0;                       /* edx */
    *(--sp) = 0;                       /* ebx */
    *(--sp) = 0;                       /* esp (dummy) */
    *(--sp) = 0;                       /* ebp */
    *(--sp) = 0;                       /* esi */
    *(--sp) = 0;                       /* edi */

    /* 段寄存器（4 个 dword，从高到低） */
    *(--sp) = 0x23;                    /* ds */
    *(--sp) = 0x23;                    /* es */
    *(--sp) = 0x23;                    /* fs */
    *(--sp) = 0x23;                    /* gs */

    /* 返回地址（trampoline） */
    *(--sp) = (uint32_t)enter_user_mode;

    /* callee-saved（4 个 dword，从高到低） */
    *(--sp) = 0;                       /* ebp */
    *(--sp) = 0;                       /* ebx */
    *(--sp) = 0;                       /* esi */
    *(--sp) = 0;                       /* edi */
    /* 此时 sp 指向栈顶（= kernel_esp） */

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
    proc_release_all_handles(task);

    /* 从就绪队列移除 */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, task);

    /* 标记 zombie，挂入 blocked_list */
    task->zombie = 1;
    task->state = TASK_STATE_ZOMBIE;
    enqueue_task(&blocked_list_head, &blocked_list_tail, task);

    /* 唤醒所有等它的进程 */
    wake_up_waiters(task);

    /* 释放 self 引用。如果 refcount 归零且 t == current，
     * proc_unref 会把 task 挂入 graveyard，由 irq_handler 回收。 */
    proc_unref(task);

    /* schedule 里 current = task，state 是 ZOMBIE，
     * 不满足"时间片耗尽"的分支，直接被跳过。
     * switch_to 会保存 task->kernel_esp 但不会再恢复。 */
    schedule();
}