#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/* IRQ 0..15 对应 int 32..47 */
#define IRQ_MAX 16

typedef void (*irq_handler_fn)(void);

/* 清零注册表。在 dev_init 之后、任何 irq_register 之前调用。 */
void irq_init(void);

/* 注册 handler。IRQ 已被占用时返回 EBUSY。 */
int irq_register(uint8_t irq, irq_handler_fn fn, const char *name);

/* 注销 handler。未注册时返回 EINVAL。 */
int irq_unregister(uint8_t irq);

/* 由 isr.c 在 irq_handler 里调用。未注册的 IRQ 静默返回。 */
void irq_dispatch(uint8_t irq);

/* 打印每个 IRQ 的触发次数（诊断用）。 */
void irq_stats(void);

#endif