#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define INPUT_QUEUE_SIZE 64

/* 事件类型 */
#define INPUT_KEY        1   /* code = ASCII 字符（含控制字符） */
#define INPUT_MOUSE_MOVE 2   /* x, y = 绝对位置 */
#define INPUT_MOUSE_BTN  3   /* code = bit0 左 / bit1 右 / bit2 中 */

struct input_event {
    uint32_t type;
    uint32_t code;
    int32_t  x, y;
};

void input_init(void);

/* 内核态投递（键盘/鼠标 IRQ 调用） */
void input_post(const struct input_event *ev);

/* 用户态读一个事件。返回 0 = 拿到，-1 = 空队列 */
int  input_poll(struct input_event *out);

/* 键盘归属——claim 后键盘 IRQ 不再写 tty 缓冲 */
void input_claim_keyboard(uint32_t pid);
void input_release_keyboard(uint32_t pid);
int  input_keyboard_claimed(void);

uint32_t input_keyboard_owner(void);

#endif