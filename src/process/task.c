#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

static uint32_t next_pid = 1;
struct task *current_task = NULL;
static struct task *ready_queue_head = NULL;
static struct task *ready_queue_tail = NULL;

/* 队列操作 */
void enqueue_task(struct task *task) {
    task->state = TASK_STATE_READY;
    task->next = NULL;
    if (ready_queue_tail) {
        ready_queue_tail->next = task;
        ready_queue_tail = task;
    } else {
        ready_queue_head = ready_queue_tail = task;
    }
}

struct task *dequeue_task(void) {
    if (!ready_queue_head) return NULL;
    struct task *task = ready_queue_head;
    ready_queue_head = task->next;
    if (!ready_queue_head) ready_queue_tail = NULL;
    task->next = NULL;
    return task;
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

    /* 构造初始内核栈（用于首次调度） */
    /* 栈布局：[返回地址] [entry_point] [user_esp] */
    // 构造 iret 帧
    uint32_t *stack_top = (uint32_t*)(task->kernel_stack_phys + 4096);
    *(--stack_top) = 0x23;                       // SS
    *(--stack_top) = task->user_stack_virt;      // ESP
    *(--stack_top) = 0x200 | 0x2;                // EFLAGS (IF=1)
    *(--stack_top) = 0x1B;                       // CS
    *(--stack_top) = task->entry_point;          // EIP
    task->kernel_esp = (uint32_t)stack_top;      // 指向EIP（正确）

    enqueue_task(task);

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
    struct task *first = dequeue_task();
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

/* 供 scheduler.c 使用的队列操作（外部可见） */
struct task *get_ready_queue_head(void) { return ready_queue_head; }
struct task *get_ready_queue_tail(void) { return ready_queue_tail; }
void set_ready_queue_head(struct task *h) { ready_queue_head = h; }
void set_ready_queue_tail(struct task *t) { ready_queue_tail = t; }


/* 释放进程的所有资源 */
void task_exit(struct task *task) {
    if (!task) return;

    kprintf("[TASK] Process %d exiting...\n", task->pid);

    /* 1. 释放页目录及所有页表（用户空间映射） */
    if (task->pgd) {
        // 遍历页目录，释放用户空间（高 3GB）的页表
        uint32_t *pgd = task->pgd;
        for (int i = KERNEL_PDE_COUNT; i < 1024; i++) {
            if (pgd[i] & PTE_PRESENT) {
                uint32_t pt_phys = pgd[i] & 0xFFFFF000;
                uint32_t *pt = (uint32_t*)pt_phys;
                // 释放页表中的物理页
                for (int j = 0; j < 1024; j++) {
                    if (pt[j] & PTE_PRESENT) {
                        uint32_t page_phys = pt[j] & 0xFFFFF000;
                        pmm_free_page(page_phys);
                    }
                }
                // 释放页表本身
                pmm_free_page(pt_phys);
            }
        }
        // 释放页目录本身
        pmm_free_page((uint32_t)task->pgd);
        task->pgd = NULL;
    }

    /* 2. 释放内核栈 */
    if (task->kernel_stack_phys) {
        pmm_free_page(task->kernel_stack_phys);
        task->kernel_stack_phys = 0;
    }

    /* 3. 清空用户栈指针 */
    task->user_stack_phys = 0;

    /* 4. 释放 PCB 本身 */
    pmm_free_page((uint32_t)task);
    set_current_task(0);

    kprintf("[TASK] Process %d resources released.\n", task->pid);
}