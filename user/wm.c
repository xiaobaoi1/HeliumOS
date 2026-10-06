/* wm — 用户态窗口管理器（骨架）
 *
 * 当前阶段：
 *   - 声明 framebuffer 和键盘
 *   - 画一个静态窗口边框
 *   - 画鼠标光标
 *   - 10 秒后退出，把屏幕还给内核 tty
 *
 * 后续：接 term、多窗口、事件路由。
 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

struct fb_user_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned int addr;
};

struct input_event {
    unsigned int type;
    unsigned int code;
    int x, y;
};

#define INPUT_KEY        1
#define INPUT_MOUSE_MOVE 2
#define INPUT_MOUSE_BTN  3

/* ---------- 基础绘制 ---------- */

static void put_pixel(unsigned int *fb, unsigned int pitch_px,
                       unsigned int W, unsigned int H,
                       int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= (int)W || y >= (int)H) return;
    fb[y * pitch_px + x] = rgb;
}

static void fill_rect(unsigned int *fb, unsigned int pitch_px,
                       unsigned int W, unsigned int H,
                       int x, int y, int w, int h, unsigned int rgb) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            put_pixel(fb, pitch_px, W, H, x + i, y + j, rgb);
        }
    }
}

/* ---------- 鼠标光标 ---------- */

#define CUR_W 12
#define CUR_H 16

/* 箭头位图——1 = 白，0 = 透明 */
static const unsigned short cursor_bits[CUR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xE000, 0xD000,
    0x8800, 0x0000, 0x0000, 0x0000,
};

static void draw_cursor(unsigned int *fb, unsigned int pitch_px,
                         unsigned int W, unsigned int H,
                         int mx, int my) {
    /* 先画白色轮廓（稍微外扩），再画黑色填充，更像箭头 */
    for (int j = 0; j < CUR_H; j++) {
        unsigned short row = cursor_bits[j];
        for (int i = 0; i < CUR_W; i++) {
            if (row & (0x8000 >> i)) {
                put_pixel(fb, pitch_px, W, H, mx + i, my + j, 0xFFFFFF);
            }
        }
    }
}

/* ---------- 屏幕布局 ---------- */

#define BG_COLOR      0x1A1A2E
#define WIN_TITLE_BG  0x3A6EA5
#define WIN_TITLE_FG  0xFFFFFF
#define WIN_BORDER    0xCCCCCC
#define WIN_BODY_BG   0x000000

static void render(unsigned int *fb, unsigned int pitch_px,
                    unsigned int W, unsigned int H,
                    int mx, int my) {
    /* 清屏 */
    fill_rect(fb, pitch_px, W, H, 0, 0, W, H, BG_COLOR);

    /* 一个假窗口 */
    int wx = 200, wy = 150;
    int ww = 600, wh = 400;

    /* 阴影 */
    fill_rect(fb, pitch_px, W, H, wx + 4, wy + 4, ww, wh, 0x000000);
    /* 标题栏 */
    fill_rect(fb, pitch_px, W, H, wx, wy, ww, 24, WIN_TITLE_BG);
    /* 边框 */
    fill_rect(fb, pitch_px, W, H, wx, wy, ww, 1, WIN_BORDER);
    fill_rect(fb, pitch_px, W, H, wx, wy + wh - 1, ww, 1, WIN_BORDER);
    fill_rect(fb, pitch_px, W, H, wx, wy, 1, wh, WIN_BORDER);
    fill_rect(fb, pitch_px, W, H, wx + ww - 1, wy, 1, wh, WIN_BORDER);
    /* 客户区 */
    fill_rect(fb, pitch_px, W, H,
              wx + 1, wy + 25, ww - 2, wh - 26, WIN_BODY_BG);

    /* 鼠标 */
    draw_cursor(fb, pitch_px, W, H, mx, my);
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct fb_user_info info;
    int r = fb_map(&info);
    if (r < 0) { printf("wm: fb_map=%d\n", r); return 1; }

    printf("wm: fb %ux%u pitch=%u bpp=%u addr=0x%x\n",
           info.width, info.height, info.pitch, info.bpp, info.addr);

    if (info.bpp != 32) {
        printf("wm: unsupported bpp\n");
        return 1;
    }

    r = fb_claim();
    if (r < 0) { printf("wm: fb_claim=%d\n", r); return 1; }
    printf("wm: fb claimed\n");

    r = input_claim();
    if (r < 0) { printf("wm: input_claim=%d\n", r); return 1; }
    printf("wm: input claimed\n");

    unsigned int *fb = (unsigned int*)info.addr;
    unsigned int pitch_px = info.pitch / 4;

    int mx = info.width / 2;
    int my = info.height / 2;
    int need_redraw = 1;

    /* 只跑 10 秒——最小验证版本 */
    for (int tick = 0; tick < 1000; tick++) {
        struct input_event ev;
        while (input_poll(&ev) == 0) {
            if (ev.type == INPUT_MOUSE_MOVE) {
                mx = ev.x;
                my = ev.y;
                need_redraw = 1;
            }
            /* 按键忽略——本阶段没有 term */
        }

        if (need_redraw) {
            render(fb, pitch_px, info.width, info.height, mx, my);
            need_redraw = 0;
        }

        sleep_ms(10);
    }

    printf("wm: exiting\n");
    return 0;
}