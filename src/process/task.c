#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

static uint32_t next_pid = 1;
struct task *current_task = NULL;

/* 队列头（全局可见） */
struct task *ready_queue_head = NULL;
struct task *ready_queue_tail = NULL;
struct task *waiting_queue_head = NULL;
struct task *waiting_queue_tail = NULL;
struct task *zombie_queue_head = NULL;
struct task *zombie_queue_tail = NULL;

/* 队列操作（通用） */
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

/* 创建进程 */
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
    task->parent = get_current_task();  // 父进程可能是 NULL（内核）

    /* 内核栈（1页） */
    task->kernel_stack_phys = pmm_alloc_page();
    if (!task->kernel_stack_phys) {
        kprintf("[TASK] ERROR: Failed to allocate kernel stack.\n");
        return NULL;
    }

    /* 用户栈（2页） */
    uint32_t stack_low = pmm_alloc_page();
    uint32_t stack_high = pmm_alloc_page();
    if (!stack_low || !stack_high) {
        kprintf("[TASK] ERROR: Failed to allocate user stack pages.\n");
        return NULL;
    }

    task->user_stack_virt = 0x7FFFE000;
    task->user_stack_phys = stack_high;

    vmm_map_user_page(pgd, 0x7FFFC000, stack_low, PTE_WRITE | PTE_USER);
    vmm_map_user_page(pgd, 0x7FFFD000, stack_high, PTE_WRITE | PTE_USER);

    /* 构造 iret 帧 */
    uint32_t *stack_top = (uint32_t*)(task->kernel_stack_phys + 4096);
    *(--stack_top) = 0x23;
    *(--stack_top) = task->user_stack_virt;
    *(--stack_top) = 0x200 | 0x2;
    *(--stack_top) = 0x1B;
    *(--stack_top) = task->entry_point;
    task->kernel_esp = (uint32_t)stack_top;

    enqueue_task(&ready_queue_head, &ready_queue_tail, task);

    kprintf("[TASK] Process %d created. Entry: %p, Kernel stack: %p\n",
            task->pid, entry_point, task->kernel_stack_phys);

    return task;
}

/* 获取/设置当前进程 */
struct task *get_current_task(void) {
    return current_task;
}

void set_current_task(struct task *task) {
    current_task = task;
    if (task) task->state = TASK_STATE_RUNNING;
}

/* 启动调度器 */
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
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %0, %%esp\n"
        "iret\n"
        :: "r"(first->kernel_esp)
        : "eax", "memory"
    );
    while (1) __asm__("cli; hlt");
}

/* 进程退出 */
void task_exit(struct task *task, int status) {
    if (!task) return;

    kprintf("[TASK] Process %d exiting with status %d\n", task->pid, status);

    task->exit_status = status;
    task->state = TASK_STATE_ZOMBIE;

    /* 释放用户空间（页表及物理页） */
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

    /* 释放内核栈 */
    if (task->kernel_stack_phys) {
        pmm_free_page(task->kernel_stack_phys);
        task->kernel_stack_phys = 0;
    }

    /* 清空用户栈指针 */
    task->user_stack_phys = 0;

    /* 从就绪队列移除（如果还在） */
    remove_task_from_queue(&ready_queue_head, &ready_queue_tail, task);

    /* 加入僵尸队列 */
    enqueue_task(&zombie_queue_head, &zombie_queue_tail, task);

    /* 唤醒父进程（如果父进程在等待） */
    if (task->parent && task->parent->state == TASK_STATE_WAITING) {
        remove_task_from_queue(&waiting_queue_head, &waiting_queue_tail, task->parent);
        task->parent->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, task->parent);
        kprintf("[TASK] Woke up parent process %d\n", task->parent->pid);
    }

    set_current_task(NULL);
    schedule();  // 让出 CPU
}