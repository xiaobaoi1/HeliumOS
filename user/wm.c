/* wm — 用户态窗口管理器（多窗口 + 幽灵框拖动 + 关闭 + 局部重绘） */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "signal.h"

/* ==================== 字体 ==================== */

static unsigned char g_glyph[128][16];
static int g_font_ok = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_font(void) {
    int fd = fs_open("SYS:/UNICODE.HEX", FS_O_RDONLY);
    if (fd < 0) { printf("wm: font open failed: %d\n", fd); return; }
    char buf[512], line[128];
    int line_len = 0, loaded = 0;
    for (;;) {
        int n = fs_read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_len == 0) continue;
                line[line_len] = '\0';
                const char *p = line;
                int cp = 0;
                while (*p && *p != ':') {
                    int v = hexval(*p);
                    if (v < 0) break;
                    cp = cp * 16 + v; p++;
                }
                if (*p == ':' && cp >= 0 && cp < 128) {
                    p++;
                    int hl = 0;
                    const char *q = p;
                    while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }
                    if (hl == 32) {
                        for (int k = 0; k < 16; k++) {
                            g_glyph[cp][k] = (hexval(p[0]) << 4) | hexval(p[1]);
                            p += 2;
                        }
                        loaded++;
                    } else if (hl == 64) {
                        for (int k = 0; k < 16; k++) {
                            g_glyph[cp][k] = hexval(p[0]) << 4;
                            p += 4;
                        }
                        loaded++;
                    }
                }
                line_len = 0;
            } else if (line_len < (int)sizeof(line) - 1) {
                line[line_len++] = c;
            }
        }
    }
    fs_close(fd);
    g_font_ok = (loaded > 0);
    printf("wm: font %d glyphs\n", loaded);
}

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

/* 从 back 恢复一块区域到 fb */
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
#define MAX_COLS 120
#define MAX_ROWS 40
#define CLOSE_BTN_W 20

#define COL_BG       0x1A1A2E
#define COL_TITLE    0x3A6EA5
#define COL_TITLE_FG 0xFFFFFF
#define COL_BORDER   0xCCCCCC
#define COL_CLIENT   0x000000
#define COL_FG       0xCCCCCC
#define COL_CLOSE    0xCC3333
#define COL_CLOSE_HL 0xFF5555

struct window {
    int used;
    int x, y, w, h;
    char title[32];
    int z;
    int pty_master, pty_slave;
    int shell_h;
    int cols, rows;
    int cx, cy;
    char buf[MAX_ROWS][MAX_COLS];
};

static struct window g_wins[MAX_WINDOWS];
static int g_next_z = 1;
static struct window *g_focus = NULL;

static int cli_x(struct window *w) { return w->x + BORDER; }
static int cli_y(struct window *w) { return w->y + TITLE_H; }
static int cli_w(struct window *w) { return w->w - 2 * BORDER; }
static int cli_h(struct window *w) { return w->h - TITLE_H - 2 * BORDER; }

static void draw_char_at(int px, int py, char c, unsigned int fg) {
    if (!g_font_ok) return;
    if ((unsigned char)c >= 128) c = '?';
    const unsigned char *g = g_glyph[(unsigned char)c];
    for (int r = 0; r < FONT_H; r++) {
        unsigned char bits = g[r];
        for (int j = 0; j < FONT_W; j++) {
            if (bits & (0x80 >> j)) put_pixel_buf(g_back, px + j, py + r, fg);
        }
    }
}

static void win_draw_char(struct window *w, int col, int row, char c) {
    int px = cli_x(w) + col * FONT_W;
    int py = cli_y(w) + row * FONT_H;
    fill_rect_buf(g_back, px, py, FONT_W, FONT_H, COL_CLIENT);
    draw_char_at(px, py, c, COL_FG);
}

static void win_scroll(struct window *w) {
    int cx = cli_x(w), cy = cli_y(w), cw = cli_w(w), ch = cli_h(w);
    for (int y = cy; y < cy + ch - FONT_H; y++) {
        unsigned int *dst = g_back + y * g_pitch_px + cx;
        unsigned int *src = g_back + (y + FONT_H) * g_pitch_px + cx;
        memcpy(dst, src, cw * 4);
    }
    fill_rect_buf(g_back, cx, cy + ch - FONT_H, cw, FONT_H, COL_CLIENT);
    for (int r = 1; r < w->rows; r++)
        memcpy(w->buf[r-1], w->buf[r], w->cols);
    memset(w->buf[w->rows-1], ' ', w->cols);
}

static void win_putc(struct window *w, char c) {
    if (c == '\n') { w->cx = 0; w->cy++; }
    else if (c == '\r') { w->cx = 0; }
    else if (c == '\b') {
        if (w->cx > 0) {
            w->cx--;
            w->buf[w->cy][w->cx] = ' ';
            win_draw_char(w, w->cx, w->cy, ' ');
        }
    } else if ((unsigned char)c >= 0x20) {
        if (w->cx < w->cols) {
            w->buf[w->cy][w->cx] = c;
            win_draw_char(w, w->cx, w->cy, c);
        }
        w->cx++;
        if (w->cx >= w->cols) { w->cx = 0; w->cy++; }
    }
    while (w->cy >= w->rows) { win_scroll(w); w->cy--; }
}

static void render_win_frame(struct window *w) {
    fill_rect_buf(g_back, w->x + 4, w->y + 4, w->w, w->h, 0x000000);
    fill_rect_buf(g_back, w->x, w->y, w->w, TITLE_H, COL_TITLE);
    fill_rect_buf(g_back, w->x, w->y, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y + w->h - BORDER, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, w->x + w->w - BORDER, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, cli_x(w), cli_y(w), cli_w(w), cli_h(w), COL_CLIENT);

    int tx = w->x + 8;
    int ty = w->y + (TITLE_H - FONT_H) / 2;
    for (const char *p = w->title; *p; p++) {
        draw_char_at(tx, ty, *p, COL_TITLE_FG);
        tx += FONT_W;
    }

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

    for (int r = 0; r < w->rows; r++) {
        for (int c = 0; c < w->cols; c++) {
            char ch = w->buf[r][c];
            if (ch == 0 || ch == ' ') continue;
            draw_char_at(cli_x(w) + c * FONT_W,
                         cli_y(w) + r * FONT_H, ch, COL_FG);
        }
    }
}

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

static int ghost_x = -1;
static int ghost_y = -1;
static int ghost_w = 0;
static int ghost_h = 0;
static int ghost_valid = 0;

static void ghost_draw(void) {
    if (!ghost_valid) return;
    int x = ghost_x, y = ghost_y, w = ghost_w, h = ghost_h;

    for (int t = 0; t < GHOST_THICK; t++) {
        /* 上 */
        for (int i = 0; i < w; i++) put_pixel_fb(x + i, y + t, GHOST_COLOR);
        /* 下 */
        for (int i = 0; i < w; i++) put_pixel_fb(x + i, y + h - 1 - t, GHOST_COLOR);
        /* 左 */
        for (int j = 0; j < h; j++) put_pixel_fb(x + t, y + j, GHOST_COLOR);
        /* 右 */
        for (int j = 0; j < h; j++) put_pixel_fb(x + w - 1 - t, y + j, GHOST_COLOR);
    }
}

static void ghost_erase(void) {
    if (!ghost_valid) return;
    fb_restore_region(ghost_x, ghost_y, ghost_w, ghost_h);
    ghost_valid = 0;
}

static void ghost_set(int x, int y, int w, int h) {
    ghost_x = x;
    ghost_y = y;
    ghost_w = w;
    ghost_h = h;
    ghost_valid = 1;
    ghost_draw();
}

/* ==================== 窗口管理 ==================== */

static struct window *make_window(int x, int y, int w, int h, const char *title) {
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) return NULL;

    struct window *win = &g_wins[slot];
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->z = g_next_z++;
    strncpy(win->title, title, sizeof(win->title) - 1);
    win->cols = cli_w(win) / FONT_W;
    win->rows = cli_h(win) / FONT_H;
    if (win->cols > MAX_COLS) win->cols = MAX_COLS;
    if (win->rows > MAX_ROWS) win->rows = MAX_ROWS;
    for (int r = 0; r < win->rows; r++) memset(win->buf[r], ' ', win->cols);

    int fds[2];
    if (pty_open(fds) < 0) { win->used = 0; return NULL; }
    win->pty_master = fds[0];
    win->pty_slave = fds[1];

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = win->pty_slave,
        .out_pty = win->pty_slave,
        .err_pty = win->pty_slave,
        .flags = 0, .envp = 0, .out_pid = 0,
    };
    win->shell_h = spawn("SHELL", NULL, &sp);
    if (win->shell_h < 0) {
        pty_close(win->pty_master);
        pty_close(win->pty_slave);
        win->used = 0;
        return NULL;
    }
    pty_set_fg(win->pty_master, sp.out_pid);
    return win;
}

static void destroy_window(struct window *w) {
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);

    if (w->shell_h >= 0) {
        kill(w->shell_h, SIGKILL);
        int st; wait(w->shell_h, &st, 200);
        process_close(w->shell_h);
    }
    if (w->pty_master >= 0) pty_close(w->pty_master);
    if (w->pty_slave >= 0) pty_close(w->pty_slave);

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

    load_font();

    struct window *w1 = make_window(80, 60, 600, 400, "Shell 1");
    struct window *w2 = make_window(360, 300, 600, 400, "Shell 2");
    if (!w1 || !w2) { printf("wm: window create failed\n"); return 1; }
    g_focus = w2;

    fill_rect_buf(g_back, 0, 0, g_W, g_H, COL_BG);
    for (int z = 0; z <= g_next_z; z++)
        for (int i = 0; i < MAX_WINDOWS; i++)
            if (g_wins[i].used && g_wins[i].z == z)
                render_win_frame(&g_wins[i]);
    blit_back_to_fb(0, 0, g_W, g_H);

    mouse_x = g_W / 2;
    mouse_y = g_H / 2;
    mouse_draw();
    printf("wm: ready\n");

    /* 拖动状态 */
    struct window *drag_win = NULL;
    int drag_dx = 0, drag_dy = 0;

    for (;;) {
        struct input_event ev;
        while (input_poll(&ev) == 0) {
            if (ev.type == INPUT_KEY) {
                if (g_focus) {
                    char c = (char)ev.code;
                    pty_write(g_focus->pty_master, &c, 1);
                }
            } else if (ev.type == INPUT_MOUSE_MOVE) {
                if (ev.x == mouse_x && ev.y == mouse_y) continue;

                if (drag_win) {
                    /* 拖动中：更新幽灵框位置 */
                    int nx = ev.x - drag_dx;
                    int ny = ev.y - drag_dy;
                    if (nx < 0) nx = 0;
                    if (ny < 0) ny = 0;
                    if (nx + drag_win->w + 4 > (int)g_W)
                        nx = g_W - drag_win->w - 4;
                    if (ny + drag_win->h + 4 > (int)g_H)
                        ny = g_H - drag_win->h - 4;

                    if (nx != ghost_x || ny != ghost_y) {
                        mouse_restore();
                        ghost_erase();
                        ghost_set(nx, ny, drag_win->w, drag_win->h);
                        mouse_x = ev.x;
                        mouse_y = ev.y;
                        mouse_draw();
                    } else {
                        /* 位置没变，只移动鼠标 */
                        mouse_restore();
                        mouse_x = ev.x;
                        mouse_y = ev.y;
                        mouse_draw();
                    }
                } else {
                    mouse_restore();
                    mouse_x = ev.x;
                    mouse_y = ev.y;
                    mouse_draw();
                }
            } else if (ev.type == INPUT_MOUSE_BTN) {
                if (ev.code & 0x01) {
                    /* 按下 */
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
                        /* 关闭按钮 */
                        destroy_window(hit);
                    } else if (ev.y >= hit->y && ev.y < hit->y + TITLE_H) {
                        /* 标题栏：开始拖动（幽灵框模式） */
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
                    /* 释放：结束拖动 */
                    if (drag_win) {
                        mouse_restore();
                        ghost_erase();

                        int old_x = drag_win->x;
                        int old_y = drag_win->y;
                        drag_win->x = ghost_x;
                        drag_win->y = ghost_y;

                        int ox0, oy0, ox1, oy1;
                        int nx0, ny0, nx1, ny1;
                        /* 旧包围盒 */
                        ox0 = old_x; oy0 = old_y;
                        ox1 = old_x + drag_win->w + 4;
                        oy1 = old_y + drag_win->h + 4;
                        /* 新包围盒 */
                        nx0 = drag_win->x; ny0 = drag_win->y;
                        nx1 = drag_win->x + drag_win->w + 4;
                        ny1 = drag_win->y + drag_win->h + 4;

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

        /* pty 输出 */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used) continue;
            int got_any = 0;
            while (pty_avail(w->pty_master) > 0) {
                char buf[128];
                int n = pty_read(w->pty_master, buf, sizeof(buf));
                if (n <= 0) break;
                for (int j = 0; j < n; j++) win_putc(w, buf[j]);
                got_any = 1;
            }
            if (got_any) {
                mouse_restore();
                redraw_region(cli_x(w), cli_y(w),
                              cli_x(w) + cli_w(w),
                              cli_y(w) + cli_h(w));
                redraw_region(w->x, w->y, w->x + w->w, w->y + TITLE_H);
                mouse_draw();
            }
        }

        sleep_ms(10);
    }
    return 0;
}