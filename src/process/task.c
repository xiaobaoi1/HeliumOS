#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

static uint32_t next_pid = 1;

/* 创建用户进程 */
struct task *task_create(uint32_t entry_point, uint32_t *pgd) {
    struct task *task = (struct task*)pmm_alloc_page();  /* 分配一页存放 PCB */
    if (!task) {
        kprintf("[TASK] ERROR: Failed to allocate PCB.\n");
        return NULL;
    }

    task->pid = next_pid++;
    task->state = TASK_STATE_READY;
    task->pgd = pgd;

    /* 分配内核栈（物理地址，4KB） */
    task->kernel_stack_phys = pmm_alloc_page();
    if (!task->kernel_stack_phys) {
        kprintf("[TASK] ERROR: Failed to allocate kernel stack.\n");
        return NULL;
    }

    /* 分配用户栈（物理地址，4KB） */
    task->user_stack_phys = pmm_alloc_page();
    if (!task->user_stack_phys) {
        kprintf("[TASK] ERROR: Failed to allocate user stack.\n");
        return NULL;
    }

    /* 用户栈虚拟地址（放在用户空间顶端往下 4KB） */
    task->user_stack_virt = 0x7FFFE000;
    task->entry_point = entry_point;

    /* 在用户页目录中映射用户栈 */
    vmm_map_user_page(pgd, task->user_stack_virt, task->user_stack_phys, PTE_WRITE | PTE_USER);

    kprintf("[TASK] Process %d created. Entry: 0x%p, User stack: 0x%p\n",
            task->pid, entry_point, task->user_stack_virt);

    return task;
}

/* 切换到用户态运行第一个进程 */
void task_run_first(struct task *task) {
    if (!task) {
        kprintf("[TASK] ERROR: No task to run.\n");
        return;
    }

    kprintf("[TASK] Switching to user process %d...\n", task->pid);

    /* 设置 TSS 内核栈（用于中断/系统调用时切换） */
    uint32_t kernel_stack_top = task->kernel_stack_phys + 4096;
    tss_set_kernel_stack(kernel_stack_top);

    /* 切换到进程的页目录 */
    uint32_t pd_phys = (uint32_t)task->pgd;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pd_phys));

    /* 构建中断返回帧（伪造一个从中断返回的现场） */
    /* 注意：这里直接使用内联汇编跳转到用户态 */

    __asm__ volatile(
        /* 禁用中断 */
        "cli\n"

        /* 设置用户态段寄存器 */
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        /* 用 iret 切换到用户态 */
        "push $0x23\n"              /* SS (用户数据段) */
        "push %0\n"                 /* ESP (用户栈) */
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"        /* 设置 IF 标志 */
        // "and $0xFFFFFDFF, %%eax\n"
        "push %%eax\n"              /* EFLAGS */
        "push $0x1B\n"              /* CS (用户代码段) */
        "push %1\n"                 /* EIP (入口点) */
        "iret\n"
        :
        : "r"(task->user_stack_virt + 4096 - 4),  /* ESP (栈顶) */
          "r"(task->entry_point)                  /* EIP */
        : "eax", "memory"
    );

    /* 永远不会执行到这里 */
}