#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

#define INPUT_QUEUE_SIZE 64

/* 事件类型 */
#define INPUT_KEY        1   /* code = ASCII 字符（含控制字符） */
#define INPUT_MOUSE_MOVE 2   /* x, y = 绝对位置 */
#define INPUT_MOUSE_BTN  3   /* code = bit0 左 / bit1 右 / bit2 中 */
#define INPUT_WHEEL      4   /* code = 滚轮增量（+ 上 / - 下） */

/* 修饰键位（mods 字段） */
#define INPUT_MOD_SHIFT  0x01
#define INPUT_MOD_CTRL   0x02
#define INPUT_MOD_ALT    0x04

struct input_event {
    uint32_t type;
    uint32_t code;
    int32_t  x, y;
    uint32_t mods;
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

/* 修饰键状态：键盘 IRQ 更新，鼠标事件/键盘事件读取 */
void     input_set_mods(uint32_t mods);
uint32_t input_get_mods(void);

#endif