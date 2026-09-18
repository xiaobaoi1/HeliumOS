#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

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
    task->parent = get_current_task();
    task->wait_target = NULL;
    task->sleep_ticks = 0;

    

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
    struct task *parent = task->parent;
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

    /* 构造内核栈（与中断布局一致） */
    uint32_t *stack_top = (uint32_t*)(task->kernel_stack_phys + 4096);

    /* iret 帧 */
    *(--stack_top) = 0x23;
    *(--stack_top) = task->user_stack_virt;
    *(--stack_top) = 0x200 | 0x2;
    *(--stack_top) = 0x1B;
    *(--stack_top) = task->entry_point;

    /* 错误码 + 中断号占位 */
    *(--stack_top) = 0;
    *(--stack_top) = 0;

    /* pusha 数据 */
    *(--stack_top) = 0;  // eax
    *(--stack_top) = 0;  // ecx
    *(--stack_top) = 0;  // edx
    *(--stack_top) = 0;  // ebx
    *(--stack_top) = 0;  // esp
    *(--stack_top) = 0;  // ebp
    *(--stack_top) = 0;  // esi
    *(--stack_top) = 0;  // edi

    /* 段寄存器 */
    *(--stack_top) = 0x23;
    *(--stack_top) = 0x23;
    *(--stack_top) = 0x23;
    *(--stack_top) = 0x23;

    task->kernel_esp = (uint32_t)stack_top;

    enqueue_task(&ready_queue_head, &ready_queue_tail, task);

    kprintf("[TASK] Process %d created. Entry: %p, Kernel stack: %p\n",
            task->pid, entry_point, task->kernel_stack_phys);

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

    __asm__ volatile("mov %0, %%cr3" :: "r"(first->pgd));
    uint32_t kernel_stack_top = first->kernel_stack_phys + 4096;
    tss_set_kernel_stack(kernel_stack_top);

    __asm__ volatile(
        "mov %0, %%esp\n"
        "pop %%gs\n"
        "pop %%fs\n"
        "pop %%es\n"
        "pop %%ds\n"
        "popa\n"
        "add $8, %%esp\n"
        "iret\n"
        :: "r"(first->kernel_esp)
        : "memory"
    );

    while (1) __asm__("cli; hlt");
}

/* ---------- 进程退出 ---------- */
void task_exit(struct task *task, int status) {
    if (!task) return;

    kprintf("[TASK] Process %d exiting with status %d\n", task->pid, status);

    task->exit_status = status;

    /* 释放文件系统句柄 */
    fs_release_all(task);
    dev_release_all(task);

    /* 释放用户空间 */
    if (task->pgd) {
        uint32_t *pgd = task->pgd;
        for (int i = KERNEL_PDE_COUNT; i < 1024; i++) {
            if (pgd[i] & PTE_PRESENT) {
                uint32_t pt_phys = pgd[i] & 0xFFFFF000;
                uint32_t *pt = (uint32_t*)pt_phys;
                for (int j = 0; j < 1024; j++) {
                    if (pt[j] & PTE_PRESENT) {
                        pmm_free_page(pt[j] & 0xFFFFF000);
                    }
                }
                pmm_free_page(pt_phys);
            }
        }
        pmm_free_page((uint32_t)task->pgd);
        task->pgd = NULL;
    }

    if (task->kernel_stack_phys) {
        pmm_free_page(task->kernel_stack_phys);
        task->kernel_stack_phys = 0;
    }
    task->user_stack_phys = 0;

    /* 从就绪队列移除 */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, task);

    /* 转入 ZOMBIE：加入 blocked_list */
    task->state = TASK_STATE_ZOMBIE;
    enqueue_task(&blocked_list_head, &blocked_list_tail, task);

    /* 唤醒所有等待它的进程 */
    wake_up_waiters(task);

    set_current_task(NULL);
    schedule();
}