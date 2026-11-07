#include <mouse.h>
#include <fb.h>
#include <io.h>
#include <irq.h>
#include <printf.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <input.h>

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

#define STATUS_OBF  0x01   /* output buffer full */
#define STATUS_IBF  0x02   /* input buffer full */

static struct mouse_state g_mouse = { 0, 0, 0, 0 };

static uint8_t g_packet[4];
static int     g_packet_idx = 0;
static int     g_packet_size = 3;   /* IntelliMouse（滚轮）为 4 */

/* ---------- 8042 通信 ---------- */

static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & STATUS_IBF)) return;
    }
}

static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & STATUS_OBF) return;
    }
}

static void ps2_write_cmd(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, cmd);
}

static void ps2_write_data(uint8_t data) {
    ps2_wait_write();
    outb(PS2_DATA, data);
}

static uint8_t ps2_read_data(void) {
    ps2_wait_read();
    return inb(PS2_DATA);
}

/* ---------- 初始化 ---------- */

/* 向鼠标发命令并读 ACK */
static void mouse_cmd(uint8_t cmd) {
    ps2_write_cmd(0xD4);
    ps2_write_data(cmd);
    (void)ps2_read_data();   /* 0xFA */
}

void mouse_init(void) {
    /* 1. 启用 auxiliary device（鼠标） */
    ps2_write_cmd(0xA8);

    /* 2. 读 config byte */
    ps2_write_cmd(0x20);
    uint8_t cfg = ps2_read_data();

    /* 3. bit1 = IRQ12 使能；bit0 = 有鼠标时清（表示鼠标存在，不是"禁用"） */
    cfg |= 0x02;
    cfg &= ~0x20;   /* bit5 = 禁用鼠标时钟：清零表示启用 */

    /* 4. 写回 config byte */
    ps2_write_cmd(0x60);
    ps2_write_data(cfg);

    /* 5. 尝试进入 IntelliMouse 模式（滚轮）：采样率 200/100/80 → 设备 ID 3。
     *    失败则退回 3 字节包（无滚轮），不影响普通移动/按键。 */
    mouse_cmd(0xF3); mouse_cmd(0xC8);   /* 200 */
    mouse_cmd(0xF3); mouse_cmd(0x64);   /* 100 */
    mouse_cmd(0xF3); mouse_cmd(0x50);   /* 80  */
    mouse_cmd(0xF2);                    /* get device ID */
    (void)ps2_read_data();              /* ACK */
    uint8_t id = ps2_read_data();
    g_packet_size = (id == 0x03) ? 4 : 3;

    /* 6. 启用数据报告 */
    mouse_cmd(0xF4);

    /* 7. 注册 IRQ 12 */
    irq_register(12, mouse_handle_irq, "mouse");

    /* 初始位置——屏幕中心 */
    struct fb_info *fb = fb_get();
    if (fb && fb->valid) {
        g_mouse.x = fb->width / 2;
        g_mouse.y = fb->height / 2;
    }

    kprintf("[MOUSE] Initialized, cursor at (%d, %d), wheel=%d\n",
            g_mouse.x, g_mouse.y, g_packet_size == 4);
}

struct mouse_state *mouse_get(void) { return &g_mouse; }

/* ---------- IRQ 处理 ---------- */

void mouse_handle_irq(void) {
    uint8_t status = inb(PS2_STATUS);
    if (!(status & STATUS_OBF)) return;
    if (!(status & 0x20)) return;   /* bit5 = 数据来自鼠标 */

    uint8_t b = inb(PS2_DATA);

    /* 第一字节必须 bit3 = 1（有效包） */
    if (g_packet_idx == 0 && !(b & 0x08)) return;

    g_packet[g_packet_idx++] = b;
    if (g_packet_idx < g_packet_size) return;
    g_packet_idx = 0;

    uint8_t flags = g_packet[0];
    int8_t  dx_raw = (int8_t)g_packet[1];
    int8_t  dy_raw = (int8_t)g_packet[2];

    /* 符号扩展：9 位有符号 */
    int dx = dx_raw;
    int dy = dy_raw;
    if (flags & 0x10) dx |= 0xFFFFFF00;   /* 已由 int8_t 处理 */
    if (flags & 0x20) dy |= 0xFFFFFF00;

    /* Y 轴反向：PS/2 向上为负，屏幕向下为正 */
    dy = -dy;

    g_mouse.x += dx;
    g_mouse.y += dy;

    /* 边界夹紧 */
    struct fb_info *fb = fb_get();
    if (fb && fb->valid) {
        if (g_mouse.x < 0) g_mouse.x = 0;
        if (g_mouse.y < 0) g_mouse.y = 0;
        if (g_mouse.x >= (int)fb->width)  g_mouse.x = fb->width  - 1;
        if (g_mouse.y >= (int)fb->height) g_mouse.y = fb->height - 1;
    }

    uint8_t prev_buttons = g_mouse.buttons;

    g_mouse.buttons = flags & 0x07;
    g_mouse.irq_count++;

        struct input_event ev_move = {
        .type = INPUT_MOUSE_MOVE,
        .code = 0,
        .x = g_mouse.x,
        .y = g_mouse.y,
        .mods = input_get_mods(),
    };
    input_post(&ev_move);

    /* 按钮变化时也投递 */
    if (g_mouse.buttons != prev_buttons) {
        struct input_event ev_btn = {
            .type = INPUT_MOUSE_BTN,
            .code = g_mouse.buttons,
            .x = g_mouse.x,
            .y = g_mouse.y,
            .mods = input_get_mods(),
        };
        input_post(&ev_btn);
    }

    /* IntelliMouse 第 4 字节：低 4 位是有符号滚轮增量 */
    if (g_packet_size == 4) {
        int wheel = (int8_t)(g_packet[3] << 4);
        wheel >>= 4;
        if (wheel != 0) {
            struct input_event ev_wheel = {
                .type = INPUT_WHEEL,
                .code = (uint32_t)wheel,
                .x = g_mouse.x,
                .y = g_mouse.y,
                .mods = input_get_mods(),
            };
            input_post(&ev_wheel);
        }
    }
}