#include <irq.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>
#include <idt.h>

struct irq_slot {
    irq_handler_fn fn;
    const char    *name;
    uint32_t       count;
};

static struct irq_slot g_slots[IRQ_MAX];

void irq_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
}

int irq_register(uint8_t irq, irq_handler_fn fn, const char *name) {
    if (irq >= IRQ_MAX || !fn) return EINVAL;
    if (g_slots[irq].fn) return EBUSY;

    g_slots[irq].fn    = fn;
    g_slots[irq].name  = name;
    g_slots[irq].count = 0;

    /* 取消 PIC 屏蔽，否则驱动收不到中断 */
    pic_unmask_irq(irq);

    kprintf("[IRQ] Registered IRQ %d: %s\n",
            irq, name ? name : "(unnamed)");
    return OK;
}

int irq_unregister(uint8_t irq) {
    if (irq >= IRQ_MAX) return EINVAL;
    if (!g_slots[irq].fn) return EINVAL;

    g_slots[irq].fn   = NULL;
    g_slots[irq].name = NULL;
    return OK;
}

void irq_dispatch(uint8_t irq) {
    if (irq >= IRQ_MAX) return;
    struct irq_slot *s = &g_slots[irq];
    s->count++;
    if (s->fn) s->fn();
}

void irq_stats(void) {
    kprintf("[IRQ] Stats:\n");
    for (int i = 0; i < IRQ_MAX; i++) {
        if (g_slots[i].count == 0 && g_slots[i].fn == NULL) continue;
        kprintf("  IRQ %2d %-16s count=%u\n",
                i,
                g_slots[i].name ? g_slots[i].name : "(unregistered)",
                g_slots[i].count);
    }
}