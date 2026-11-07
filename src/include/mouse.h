#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>

struct mouse_state {
    int      x, y;         /* 屏幕坐标 */
    uint8_t  buttons;      /* bit0 = 左, bit1 = 右, bit2 = 中 */
    uint32_t irq_count;
};

void mouse_init(void);
struct mouse_state *mouse_get(void);

/* 由 irq_dispatch 调用 */
void mouse_handle_irq(void);

#endif