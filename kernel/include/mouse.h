#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

struct mouse_state {
    int      x, y;
    uint8_t  buttons;
    uint32_t irq_count;
};

void mouse_init(void);
struct mouse_state *mouse_get(void);

/* 处理一个鼠标数据字节（来自 IRQ 12，或键盘 IRQ 转交）。
 * 不做 8042 端口读写，纯状态机。 */
void mouse_handle_byte(uint8_t b);

/* 由 irq_dispatch 调用 */
void mouse_handle_irq(void);

#endif