#include <isr.h>
#include <syscall.h>
#include <scheduler.h>
#include <task.h>
#include <printf.h>
#include <stddef.h>
#include <keyboard.h>
#include <io.h>
#include <signal.h>
#include <panic.h>
#include <extable.h>
#include <vma.h>
#include <vmm.h>

extern void sleep_tick(void);


/* 页错误按需分配。
 * 覆盖：
 *   - 用户态访问 lazy 页（brk 扩展 / VMA 未映射）
 *   - 内核态 copy_from_user/copy_to_user 访问 lazy 用户页
 * 返回 0 表示已映射（iret 重试），-1 表示无法恢复。 */
static int handle_page_fault(struct task *cur, uint32_t fault_addr,
                              uint32_t err_code) {
    int present = err_code & 1;
    int write   = err_code & 2;
    int user    = err_code & 4;

    /* 内核态访问内核地址 fault——不是用户内存 */
    if (!user && fault_addr < USER_SPACE_START) return -1;

    /* 页已映射但权限不足——无法按需修复 */
    if (present) return -1;

    struct vma *v = vma_find(cur, fault_addr);
    int is_heap = (fault_addr >= cur->heap_base &&
                   fault_addr < cur->heap_brk);
    if (!v && !is_heap) return -1;

    /* 用户态访问检查 VMA 权限；内核态访问用户地址信任调用方 */
    if (user && v) {
        if (write && !(v->flags & VMA_WRITE)) return -1;
        if (!write && !(v->flags & VMA_READ)) return -1;
    }

    uint32_t phys = pmm_alloc_page();
    if (!phys) return -1;
    memset((void*)phys, 0, 4096);

    uint32_t page = fault_addr & ~0xFFF;
    uint32_t pte_flags = PTE_USER;
    if (v) {
        if (v->flags & VMA_WRITE) pte_flags |= PTE_WRITE;
    } else {
        pte_flags |= PTE_WRITE;   /* heap 总是可写 */
    }

    vmm_map_user_page(cur->pgd, page, phys, pte_flags);
    return 0;
}

/* 异常处理 */
void isr_handler(struct registers *regs) {
    uint32_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    int from_user = (regs->cs & 3) == 3;
    struct task *cur = get_current_task();

    /* page fault：先尝试按需分配 */
    if (regs->int_no == 14 && cur) {
        if (handle_page_fault(cur, cr2, regs->err_code) == 0) {
            return;
        }
    }

    if (from_user) {
        if (!cur) {
            panic_regs(regs, "user exception without task");
        }

        int sig = signal_from_exception(regs->int_no);

        kprintf("[USER FAULT] pid=%d int=%d sig=%d cr2=0x%x eip=0x%x\n",
                (int)cur->pid, (int)regs->int_no, sig, cr2, regs->eip);

        struct sig_action *a = &cur->sig_actions[sig];
        if (a->handler == SIG_DFL || a->handler == SIG_IGN) {
            task_terminate(cur, 128 + sig);
            proc_unref(cur);
            schedule();
            return;
        }
        cur->pending_signals |= (1u << sig);
        signal_deliver_pending(regs, cur);
        return;
    }

    /* 内核态异常：page fault 先查异常表 */
    if (regs->int_no == 14) {
        uint32_t fixup = extable_lookup(regs->eip);
        if (fixup) {
            regs->eip = fixup;
            return;
        }
    }

    panic_regs(regs, "kernel exception int=%d err=%p",
               (int)regs->int_no, regs->err_code);
}

/* 硬件中断处理 */
void irq_handler(struct registers *regs) {
    /* EOI 先发——让 PIC 可以接受新中断。
     * 从 PIC 的 IRQ（>= 40）额外发从 PIC EOI。 */
    if (regs->int_no >= 40) {
        __asm__ volatile("mov $0x20, %%al; out %%al, $0xA0" ::: "eax", "memory");
    }
    __asm__ volatile("mov $0x20, %%al; out %%al, $0x20" ::: "eax", "memory");

    if (regs->int_no == 32) {
        /* IRQ 0（PIT）：调度器心跳。保留硬编码——
         * 它必须在 irq_handler 里直接处理，不能走注册表。 */
        proc_reap_graveyard();
        sleep_tick();

        struct task *current = get_current_task();
        if (current) {
            current->time_slice--;
            if (current->time_slice == 0) {
                schedule();
            }
        } else {
            schedule();
        }
    } else if (regs->int_no >= 33 && regs->int_no <= 47) {
        /* IRQ 1..15：分发到注册表 */
        irq_dispatch((uint8_t)(regs->int_no - 32));
    }

    struct task *cur = get_current_task();
    if (cur) {
        signal_deliver_pending(regs, cur);
    }
}

/* 系统调用处理入口 */
void isr_syscall_handler(struct registers *regs) {
    syscall_handler(regs);
}