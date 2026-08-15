#include <task.h>
#include <pmm.h>
#include <vmm.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

static uint32_t next_pid = 1;

struct task *task_create(uint32_t entry_point, uint32_t *pgd) {
    struct task *task = (struct task*)pmm_alloc_page();
    if (!task) {
        kprintf("[TASK] ERROR: Failed to allocate PCB.\n");
        return NULL;
    }

    task->pid = next_pid++;
    task->state = TASK_STATE_READY;
    task->pgd = pgd;
    task->entry_point = entry_point;

    // 内核栈（1页）
    task->kernel_stack_phys = pmm_alloc_page();
    if (!task->kernel_stack_phys) {
        kprintf("[TASK] ERROR: Failed to allocate kernel stack.\n");
        return NULL;
    }

    // ===== 用户栈：分配两页物理内存，映射到连续虚拟地址 =====
    // 虚拟地址范围：0x7FFFC000 ~ 0x7FFFE000（8KB 栈空间）
    // 栈顶在 0x7FFFE000，向下增长，栈底在 0x7FFFC000
    // 下方 0x7FFFB000 未映射，作为保护页

    uint32_t stack_phys_low = pmm_alloc_page();   // 映射到 0x7FFFC000
    uint32_t stack_phys_high = pmm_alloc_page();  // 映射到 0x7FFFD000
    if (!stack_phys_low || !stack_phys_high) {
        kprintf("[TASK] ERROR: Failed to allocate user stack pages.\n");
        return NULL;
    }

    // 用户栈虚拟地址（栈顶）
    task->user_stack_virt = 0x7FFFE000;
    task->user_stack_phys = stack_phys_high;  // 仅保存高页地址（用于后续释放）

    // 映射两页到用户空间
    vmm_map_user_page(pgd, 0x7FFFC000, stack_phys_low, PTE_WRITE | PTE_USER);
    vmm_map_user_page(pgd, 0x7FFFD000, stack_phys_high, PTE_WRITE | PTE_USER);

    kprintf("[TASK] Process %d created. Entry: 0x%p, User stack: 0x%p (8KB, guard page below), Kernel Stack: 0x%p\n",
            task->pid, entry_point, task->user_stack_virt, task->kernel_stack_phys);

    return task;
}

void task_run_first(struct task *task) {
    if (!task) return;

    kprintf("[TASK] Switching to user process %d...(Entry: %p)\n", task->pid, task->entry_point);

    uint32_t kernel_stack_top = task->kernel_stack_phys + 4096;
    tss_set_kernel_stack(kernel_stack_top);

    uint32_t pd_phys = (uint32_t)task->pgd;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pd_phys));

    // 栈顶（向下增长，初始 esp 指向栈顶）
    uint32_t user_esp = task->user_stack_virt;

    kprintf("[TASK] Before iret: entry_point=0x%x, user_esp=0x%x, kernel_stack=0x%x\n",
        task->entry_point, user_esp, kernel_stack_top);

    __asm__ volatile(
        "cli\n"
        "mov $0x23, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "push $0x23\n"              /* SS */
        "push %0\n"                 /* ESP */
        "pushf\n"
        "pop %%eax\n"
        "or $0x200, %%eax\n"        /* IF = 1 */
        "push %%eax\n"              /* EFLAGS */
        "push $0x1B\n"              /* CS */
        "push %1\n"                 /* EIP */
        "iret\n"
        :
        : "r"(user_esp),
          "r"(task->entry_point)
        : "eax", "memory"
    );
    while (1); /* never reached */
}