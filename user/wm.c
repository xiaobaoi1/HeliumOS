/* wm — 窗口管理器（term 分离版）
 *
 * 职责：
 *   - framebuffer + 输入声明
 *   - 窗口装饰（标题栏、边框、关闭按钮）
 *   - 合成（term 写 shm，WM 从 shm blit）
 *   - 焦点 + z-order + 拖动（幽灵框）
 *   - 鼠标光标
 *   - 事件路由（按键 → 焦点窗口的 event pipe）
 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>
#include "signal.h"

/* 前向声明 */
static void mouse_restore(void);
static void mouse_draw(void);

/* ==================== 屏幕 ==================== */

static unsigned int *g_fb;
static unsigned int *g_back;
static unsigned int  g_W, g_H, g_pitch_px;

static void put_pixel_buf(unsigned int *buf, int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= (int)g_W || y >= (int)g_H) return;
    buf[y * g_pitch_px + x] = rgb;
}

static void put_pixel_fb(int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= (int)g_W || y >= (int)g_H) return;
    g_fb[y * g_pitch_px + x] = rgb;
}

static void fill_rect_buf(unsigned int *buf, int x, int y, int w, int h,
                           unsigned int rgb) {
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            put_pixel_buf(buf, x + i, y + j, rgb);
}

static void fb_restore_region(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= (int)g_H) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= (int)g_W) continue;
            g_fb[py * g_pitch_px + px] = g_back[py * g_pitch_px + px];
        }
    }
}

static void blit_back_to_fb(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_W) x1 = (int)g_W;
    if (y1 > (int)g_H) y1 = (int)g_H;
    if (x1 <= x0 || y1 <= y0) return;
    for (int y = y0; y < y1; y++) {
        unsigned int *src = g_back + y * g_pitch_px + x0;
        unsigned int *dst = g_fb   + y * g_pitch_px + x0;
        memcpy(dst, src, (x1 - x0) * 4);
    }
}

/* ==================== 窗口 ==================== */

#define MAX_WINDOWS 4
#define TITLE_H 24
#define BORDER 2
#define FONT_W 8
#define FONT_H 16
#define CLOSE_BTN_W 20

#define COL_BG       0x1A1A2E
#define COL_TITLE    0x3A6EA5
#define COL_TITLE_FG 0xFFFFFF
#define COL_BORDER   0xCCCCCC
#define COL_CLOSE    0xCC3333
#define COL_CLOSE_HL 0xFF5555

struct window {
    int used;
    int x, y, w, h;
    char title[32];
    int z;

    int term_h;
    int term_pid;

    int shm_idx;
    unsigned int *pixels;    /* WM 侧 shm 映射（像素部分） */
    uint64_t *counter;       /* shm 头部 */
    uint64_t last_seen;

    int event_w;             /* WM 保留的事件 pipe 写端 */
};

static struct window g_wins[MAX_WINDOWS];
static int g_next_z = 1;
static struct window *g_focus = NULL;

static int cli_x(struct window *w) { return w->x + BORDER; }
static int cli_y(struct window *w) { return w->y + TITLE_H; }
static int cli_w(struct window *w) { return w->w - 2 * BORDER; }
static int cli_h(struct window *w) { return w->h - TITLE_H - 2 * BORDER; }

/* 从 shm 拷贝像素到 back 的客户区 */
static void blit_shm_to_back(struct window *w) {
    int cw = cli_w(w);
    int ch = cli_h(w);
    int cx = cli_x(w);
    int cy = cli_y(w);

    for (int y = 0; y < ch; y++) {
        unsigned int *dst = g_back + (cy + y) * g_pitch_px + cx;
        unsigned int *src = w->pixels + y * cw;
        memcpy(dst, src, cw * 4);
    }
}

/* 前向声明 */
static void draw_title_text(struct window *w);

/* 绘制窗口装饰（不含客户区内容） */
static void render_win_frame(struct window *w) {
    /* 阴影 */
    fill_rect_buf(g_back, w->x + 4, w->y + 4, w->w, w->h, 0x000000);

    /* 标题栏 */
    fill_rect_buf(g_back, w->x, w->y, w->w, TITLE_H, COL_TITLE);

    /* 边框 */
    fill_rect_buf(g_back, w->x, w->y, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y + w->h - BORDER, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, w->x + w->w - BORDER, w->y, BORDER, w->h, COL_BORDER);

    /* 客户区黑底（term 会覆盖） */
    fill_rect_buf(g_back, cli_x(w), cli_y(w), cli_w(w), cli_h(w), 0x000000);

    /* 标题文字 */
    draw_title_text(w);

    /* 关闭按钮 */
    int bx = w->x + w->w - CLOSE_BTN_W - 4;
    int by = w->y + 4;
    unsigned int btn_col = (w == g_focus) ? COL_CLOSE_HL : COL_CLOSE;
    fill_rect_buf(g_back, bx, by, CLOSE_BTN_W - 8, CLOSE_BTN_W - 8, btn_col);
    int cx0 = bx + 2, cy0 = by + 2;
    int sz = CLOSE_BTN_W - 12;
    for (int i = 0; i < sz; i++) {
        put_pixel_buf(g_back, cx0 + i, cy0 + i, 0xFFFFFF);
        put_pixel_buf(g_back, cx0 + sz - 1 - i, cy0 + i, 0xFFFFFF);
        put_pixel_buf(g_back, cx0 + i + 1, cy0 + i, 0xFFFFFF);
        put_pixel_buf(g_back, cx0 + sz - i, cy0 + i, 0xFFFFFF);
    }
}

/* 简单画字——WM 只画标题。用内嵌最小 8x16 字模（只支持 A-Z 0-9）。 */
static const unsigned char g_title_glyph[] = {
    /* 空白留待将来。当前标题文字先不画。 */
};

static void draw_title_text(struct window *w) {
    (void)w;
    /* TODO：WM 需要画标题文字。选项：
     *  A) 内嵌一份最小字模
     *  B) 从 shm 读字体（term 写字体到某个共享区）
     *  C) 不画标题文字
     * 当前选 C——先跑通架构，标题文字后加。 */
}

/* 区域重绘 */
static void redraw_region(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_W) x1 = (int)g_W;
    if (y1 > (int)g_H) y1 = (int)g_H;
    if (x1 <= x0 || y1 <= y0) return;

    fill_rect_buf(g_back, x0, y0, x1 - x0, y1 - y0, COL_BG);

    for (int z = 0; z <= g_next_z; z++) {
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used || w->z != z) continue;
            if (w->x + 4 >= x1 || w->x + w->w + 4 <= x0) continue;
            if (w->y + 4 >= y1 || w->y + w->h + 4 <= y0) continue;
            render_win_frame(w);
            blit_shm_to_back(w);
        }
    }

    blit_back_to_fb(x0, y0, x1, y1);
}

static void win_bbox(struct window *w, int *x0, int *y0, int *x1, int *y1) {
    *x0 = w->x;
    *y0 = w->y;
    *x1 = w->x + w->w + 4;
    *y1 = w->y + w->h + 4;
}

/* ==================== 鼠标 ==================== */

#define CUR_W 12
#define CUR_H 16

static const unsigned short cursor_bits[CUR_H] = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xFF80, 0xFFC0, 0xE000, 0xD000,
    0x8800, 0x0000, 0x0000, 0x0000,
};

static int mouse_x = 0, mouse_y = 0;
static int mouse_prev_x = -1, mouse_prev_y = -1;

static void mouse_restore(void) {
    if (mouse_prev_x < 0) return;
    fb_restore_region(mouse_prev_x, mouse_prev_y, CUR_W, CUR_H);
    mouse_prev_x = -1;
    mouse_prev_y = -1;
}

static void mouse_draw(void) {
    for (int j = 0; j < CUR_H; j++) {
        unsigned short row = cursor_bits[j];
        int py = mouse_y + j;
        if (py < 0 || py >= (int)g_H) continue;
        for (int i = 0; i < CUR_W; i++) {
            if (!(row & (0x8000 >> i))) continue;
            int px = mouse_x + i;
            if (px < 0 || px >= (int)g_W) continue;
            g_fb[py * g_pitch_px + px] = 0xFFFFFF;
        }
    }
    mouse_prev_x = mouse_x;
    mouse_prev_y = mouse_y;
}

/* ==================== 幽灵框 ==================== */

#define GHOST_THICK 2
#define GHOST_COLOR 0xFFFFFF

static int ghost_x = -1, ghost_y = -1, ghost_w = 0, ghost_h = 0;
static int ghost_valid = 0;

static void ghost_draw(void) {
    if (!ghost_valid) return;
    int x = ghost_x, y = ghost_y, w = ghost_w, h = ghost_h;
    for (int t = 0; t < GHOST_THICK; t++) {
        for (int i = 0; i < w; i++) {
            put_pixel_fb(x + i, y + t, GHOST_COLOR);
            put_pixel_fb(x + i, y + h - 1 - t, GHOST_COLOR);
        }
        for (int j = 0; j < h; j++) {
            put_pixel_fb(x + t, y + j, GHOST_COLOR);
            put_pixel_fb(x + w - 1 - t, y + j, GHOST_COLOR);
        }
    }
}

static void ghost_erase(void) {
    if (!ghost_valid) return;
    fb_restore_region(ghost_x, ghost_y, ghost_w, ghost_h);
    ghost_valid = 0;
}

static void ghost_set(int x, int y, int w, int h) {
    ghost_x = x; ghost_y = y; ghost_w = w; ghost_h = h;
    ghost_valid = 1;
    ghost_draw();
}

/* ==================== 窗口管理 ==================== */

static struct window *make_window(int x, int y, int w, int h,
                                   const char *title, int slot) {
    struct window *win = &g_wins[slot];
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->z = g_next_z++;
    strncpy(win->title, title, sizeof(win->title) - 1);

    int cw = cli_w(win);
    int ch = cli_h(win);

    /* 创建 shm */
    char shm_name[16];
    snprintf(shm_name, sizeof(shm_name), "term-%d", slot);

    int shm_size = 8 + cw * ch * 4;
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) { win->used = 0; return NULL; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { win->used = 0; return NULL; }

    win->shm_idx = shm_id;
    win->counter = (uint64_t*)addr;
    win->pixels = (unsigned int*)(addr + 8);
    *win->counter = 0;
    win->last_seen = 0;
    memset(win->pixels, 0, cw * ch * 4);

    /* 事件 pipe（WM 保留写端） */
    int fds[2];
    if (pipe(fds) < 0) { win->used = 0; return NULL; }
    win->event_w = fds[1];

    /* spawn TERM */
    char arg_w[8], arg_h[8];
    snprintf(arg_w, sizeof(arg_w), "%d", cw);
    snprintf(arg_h, sizeof(arg_h), "%d", ch);

    char *argv[] = { "TERM", shm_name, arg_w, arg_h, NULL };

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = fds[0],           /* term 的 stdin = 事件 pipe 读端 */
        .out_pipe = -1,
        .err_pipe = -1,
        .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0,
    };

    win->term_h = spawn("TERM", argv, &sp);
    if (win->term_h < 0) {
        ipc_close(fds[0]);
        ipc_close(fds[1]);
        win->used = 0;
        return NULL;
    }
    win->term_pid = sp.out_pid;

    /* WM 关掉自己那份读端——只保留写端 */
    ipc_close(fds[0]);

    return win;
}

static void destroy_window(struct window *w) {
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);

    if (w->term_h >= 0) {
        kill(w->term_h, SIGKILL);
        int st; wait(w->term_h, &st, 200);
        process_close(w->term_h);
    }
    if (w->event_w >= 0) ipc_close(w->event_w);

    /* shm 引用释放——term 退出后由 shm_unlink 清 */
    char shm_name[16];
    snprintf(shm_name, sizeof(shm_name), "term-%d", w - g_wins);
    shm_unlink(shm_name);

    w->used = 0;
    if (g_focus == w) g_focus = NULL;

    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
}

/* ==================== main ==================== */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    struct fb_user_info info;
    if (fb_map(&info) < 0) { printf("wm: fb_map failed\n"); return 1; }
    if (info.bpp != 32) { printf("wm: bpp %u unsupported\n", info.bpp); return 1; }
    if (fb_claim() < 0) { printf("wm: fb_claim failed\n"); return 1; }
    if (input_claim() < 0) { printf("wm: input_claim failed\n"); return 1; }

    g_fb = (unsigned int*)info.addr;
    g_W  = info.width;
    g_H  = info.height;
    g_pitch_px = info.pitch / 4;

    unsigned int back_size = info.pitch * info.height;
    unsigned int back_addr = mmap(0, back_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1);
    if ((int)back_addr <= 0) { printf("wm: back mmap failed\n"); return 1; }
    g_back = (unsigned int*)back_addr;
    memset(g_back, 0, back_size);

    struct window *w1 = make_window(80, 60, 600, 400, "Shell 1", 0);
    struct window *w2 = make_window(360, 300, 600, 400, "Shell 2", 1);
    if (!w1 || !w2) { printf("wm: window create failed\n"); return 1; }
    g_focus = w2;

    fill_rect_buf(g_back, 0, 0, g_W, g_H, COL_BG);
    for (int z = 0; z <= g_next_z; z++)
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g_wins[i].used && g_wins[i].z == z) {
                render_win_frame(&g_wins[i]);
                blit_shm_to_back(&g_wins[i]);
            }
    blit_back_to_fb(0, 0, g_W, g_H);

    mouse_x = g_W / 2;
    mouse_y = g_H / 2;
    mouse_draw();
    printf("wm: ready\n");

    struct window *drag_win = NULL;
    int drag_dx = 0, drag_dy = 0;

    for (;;) {
        struct input_event ev;
        while (input_poll(&ev) == 0) {
            if (ev.type == INPUT_KEY) {
                if (g_focus && g_focus->event_w >= 0) {
                    char c = (char)ev.code;
                    pipe_write(g_focus->event_w, &c, 1);
                }
            } else if (ev.type == INPUT_MOUSE_MOVE) {
                if (ev.x == mouse_x && ev.y == mouse_y) continue;

                if (drag_win) {
                    int nx = ev.x - drag_dx;
                    int ny = ev.y - drag_dy;
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx + drag_win->w + 4 > (int)g_W)
                        nx = g_W - drag_win->w - 4;
                    if (ny + drag_win->h + 4 > (int)g_H)
                        ny = g_H - drag_win->h - 4;

                    mouse_restore();
                    ghost_erase();
                    ghost_set(nx, ny, drag_win->w, drag_win->h);
                    mouse_x = ev.x; mouse_y = ev.y;
                    mouse_draw();
                } else {
                    mouse_restore();
                    mouse_x = ev.x; mouse_y = ev.y;
                    mouse_draw();
                }
            } else if (ev.type == INPUT_MOUSE_BTN) {
                if (ev.code & 0x01) {
                    struct window *hit = NULL;
                    int best_z = -1;
                    for (int i = 0; i < MAX_WINDOWS; i++) {
                        struct window *w = &g_wins[i];
                        if (!w->used) continue;
                        if (ev.x >= w->x && ev.x < w->x + w->w &&
                            ev.y >= w->y && ev.y < w->y + w->h) {
                            if (w->z > best_z) { best_z = w->z; hit = w; }
                        }
                    }
                    if (!hit) continue;

                    int need_full = 0;
                    if (hit != g_focus) { g_focus = hit; need_full = 1; }
                    hit->z = g_next_z++;

                    int bx = hit->x + hit->w - CLOSE_BTN_W - 4;
                    int by = hit->y + 4;
                    if (ev.x >= bx && ev.x < bx + CLOSE_BTN_W &&
                        ev.y >= by && ev.y < by + CLOSE_BTN_W) {
                        destroy_window(hit);
                    } else if (ev.y >= hit->y && ev.y < hit->y + TITLE_H) {
                        drag_win = hit;
                        drag_dx = ev.x - hit->x;
                        drag_dy = ev.y - hit->y;
                        if (need_full) {
                            mouse_restore();
                            redraw_region(0, 0, g_W, g_H);
                            mouse_draw();
                        }
                    } else if (need_full) {
                        mouse_restore();
                        redraw_region(0, 0, g_W, g_H);
                        mouse_draw();
                    }
                } else {
                    if (drag_win) {
                        mouse_restore();
                        ghost_erase();

                        int old_x = drag_win->x, old_y = drag_win->y;
                        drag_win->x = ghost_x;
                        drag_win->y = ghost_y;

                        int ox0 = old_x, oy0 = old_y;
                        int ox1 = old_x + drag_win->w + 4;
                        int oy1 = old_y + drag_win->h + 4;
                        int nx0 = drag_win->x, ny0 = drag_win->y;
                        int nx1 = drag_win->x + drag_win->w + 4;
                        int ny1 = drag_win->y + drag_win->h + 4;
                        int ux0 = ox0 < nx0 ? ox0 : nx0;
                        int uy0 = oy0 < ny0 ? oy0 : ny0;
                        int ux1 = ox1 > nx1 ? ox1 : nx1;
                        int uy1 = oy1 > ny1 ? oy1 : ny1;

                        redraw_region(ux0, uy0, ux1, uy1);
                        mouse_draw();
                        drag_win = NULL;
                    }
                }
            }
        }

        // static int wc = 0;
        // if (wc++ < 30) {
        //     dprintf(-1, "wm: c0=%llu l0=%llu c1=%llu l1=%llu\n",
        //             w1 ? *w1->counter : 0, w1 ? w1->last_seen : 0,
        //             w2 ? *w2->counter : 0, w2 ? w2->last_seen : 0);
        // }

        /* 检查 term 更新——counter 变化才重绘 */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used) continue;
            if (*w->counter == w->last_seen) {
                g_fb[0] = (*w->counter & 1) ? 0xFF0000 : 0x00FF00;
                continue;
            }

            w->last_seen = *w->counter;
            mouse_restore();
            blit_shm_to_back(w);
            blit_back_to_fb(cli_x(w), cli_y(w),
                            cli_x(w) + cli_w(w),
                            cli_y(w) + cli_h(w));
            mouse_draw();
        }

        sleep_ms(10);
    }

    return 0;
}