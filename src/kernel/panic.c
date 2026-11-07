#include <panic.h>
#include <printf.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

static void dump_regs(const struct registers *r) {
    if (!r) return;
    kprintf("--- registers ---\n");
    kprintf("  eax=%p ebx=%p ecx=%p edx=%p\n",
            r->eax, r->ebx, r->ecx, r->edx);
    kprintf("  esi=%p edi=%p ebp=%p esp=%p\n",
            r->esi, r->edi, r->ebp, r->esp);
    kprintf("  eip=%p cs=%x eflags=%p\n",
            r->eip, r->cs, r->eflags);
    kprintf("  int_no=%d err_code=%p\n",
            (int)r->int_no, r->err_code);
    kprintf("  user_esp=%p user_ss=%x\n",
            r->user_esp, r->user_ss);
}

static void halt_forever(void) {
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void panic(const char *fmt, ...) {
    kprintf("\n[PANIC] ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    uint32_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kprintf("  cr2=%p cr3=%p\n", cr2, cr3);

    kprintf("[PANIC] System halted.\n");
    halt_forever();
}

void panic_regs(const struct registers *regs, const char *fmt, ...) {
    kprintf("\n[PANIC] ");
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");

    dump_regs(regs);

    uint32_t cr2, cr3;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    kprintf("  cr2=%p cr3=%p\n", cr2, cr3);

    kprintf("[PANIC] System halted.\n");
    halt_forever();
}