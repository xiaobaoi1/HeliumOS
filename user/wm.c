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
#include "wm_proto.h"
#include "shm_layout.h"

/* 前向声明 */
static void mouse_restore(void);
static void mouse_draw(void);

/* ==================== 标题字体 ==================== */

static unsigned char g_title_glyph[128][16];
static int g_title_font_ok = 0;

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void load_title_font(void) {
    int fd = fs_open("SYS:/UNICODE.HEX", FS_O_RDONLY);
    if (fd < 0) return;

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
                    cp = cp * 16 + v;
                    p++;
                }
                if (*p == ':' && cp >= 0 && cp < 128) {
                    p++;
                    int hl = 0;
                    const char *q = p;
                    while (hexval(q[0]) >= 0 && hexval(q[1]) >= 0) { hl += 2; q += 2; }
                    if (hl == 32) {
                        for (int k = 0; k < 16; k++) {
                            g_title_glyph[cp][k] = (hexval(p[0]) << 4) | hexval(p[1]);
                            p += 2;
                        }
                        loaded++;
                    } else if (hl == 64) {
                        for (int k = 0; k < 16; k++) {
                            g_title_glyph[cp][k] = hexval(p[0]) << 4;
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
    g_title_font_ok = (loaded > 0);
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

    int is_term;              /* 1 = WM spawn 的 TERM，0 = 应用自建 */
    int owner_pid;            /* 存活检测 */
    char shm_name[16];        /* 仅应用窗口——unlink 时用 */

    int shm_idx;
    unsigned int *pixels;    /* WM 侧 shm 映射（像素部分） */
    uint64_t *counter;       /* shm 头部 */
    uint64_t last_seen;
    struct shm_hdr *shm;     /* 新 */

    int event_w;             /* WM 保留的事件 pipe 写端 */
};

static int g_inbox_rd = -1;
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

static void draw_title_text(struct window *w) {
    if (!g_title_font_ok) return;

    int tx = w->x + 8;
    int ty = w->y + (TITLE_H - FONT_H) / 2;

    for (const char *p = w->title; *p; p++) {
        int cp = (unsigned char)*p;
        if (cp >= 128) cp = '?';

        const unsigned char *g = g_title_glyph[cp];
        for (int r = 0; r < FONT_H; r++) {
            unsigned char bits = g[r];
            for (int j = 0; j < FONT_W; j++) {
                if (bits & (0x80 >> j)) {
                    put_pixel_buf(g_back, tx + j, ty + r, COL_TITLE_FG);
                }
            }
        }
        tx += FONT_W;

        /* 超出标题栏（预留关闭按钮 24px）停止 */
        if (tx + FONT_W > w->x + w->w - 28) break;
    }
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

    /* 创建窗口 shm */
    char shm_name[16];
    snprintf(shm_name, sizeof(shm_name), "term-%d", slot);

    strncpy(win->shm_name, shm_name, sizeof(win->shm_name) - 1);
    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) { win->used = 0; return NULL; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { win->used = 0; return NULL; }

    win->shm_idx = shm_id;
    win->shm = (struct shm_hdr*)addr;
    win->counter = &win->shm->counter;
    win->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    win->last_seen = 0;
    memset(win->pixels, 0, cw * ch * 4);

    /* 事件 pipe——命名 pipe。
     * 先 unlink 防止上次残留（比如 WM 异常退出）。 */
    char ev_name[24];
    snprintf(ev_name, sizeof(ev_name), "wm-term-%d", slot);
    pipe_unlink(ev_name);   /* 忽略 ENOENT */

    int fds[2];
    if (pipe_named(ev_name, fds) < 0) { win->used = 0; return NULL; }
    win->event_w = fds[1];
    ipc_close(fds[0]);   /* WM 保留写端，读端留给 term */

    /* spawn TERM——事件 pipe 名字作为参数传 */
    char arg_w[8], arg_h[8];
    snprintf(arg_w, sizeof(arg_w), "%d", cw);
    snprintf(arg_h, sizeof(arg_h), "%d", ch);

    char *argv[] = { "TERM", shm_name, arg_w, arg_h, ev_name, NULL };

    struct spawn_params sp = {
        .size = sizeof(sp),
        .in_fd = SPAWN_FD_INHERIT,
        .out_fd = SPAWN_FD_INHERIT,
        .err_fd = SPAWN_FD_INHERIT,
        .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
        .in_pty = -1, .out_pty = -1, .err_pty = -1,
        .flags = 0, .envp = 0, .out_pid = 0,
        .child_class = CLASS_SERVICE,      /* 新增 */
        .child_caps  = 0,                  /* TERM 不需要特权 */
    };

    win->term_h = spawn("TERM", argv, &sp);
    if (win->term_h < 0) {
        ipc_close(win->event_w);
        pipe_unlink(ev_name);
        win->used = 0;
        return NULL;
    }
    win->term_pid = sp.out_pid;

    win->is_term = 1;
    win->owner_pid = sp.out_pid;

    return win;
}

static struct window *make_app_window(struct wm_request *req) {
    if (req->w == 0 || req->h == 0) return NULL;
    if (req->w > 1600 || req->h > 1200) return NULL;

    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) return NULL;

    struct window *w = &g_wins[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->x = 120 + slot * 40;
    w->y = 120 + slot * 40;
    w->w = req->w + 2 * BORDER;
    w->h = req->h + TITLE_H + 2 * BORDER;
    w->z = g_next_z++;
    w->is_term = 0;
    w->owner_pid = req->owner_pid;
    strncpy(w->title, req->title, sizeof(w->title) - 1);
    strncpy(w->shm_name, req->shm_name, sizeof(w->shm_name) - 1);

    /* 打开应用 shm */
    int shm_size = shm_total_size(req->w, req->h);
    int shm_id = shm_open(req->shm_name, shm_size, 0);
    if (shm_id < 0) { w->used = 0; return NULL; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { w->used = 0; return NULL; }

    w->shm_idx = shm_id;
    w->shm = (struct shm_hdr*)addr;
    w->counter = &w->shm->counter;
    w->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    w->last_seen = *w->counter;   /* 跳过首帧，等应用主动 counter++ */

    /* 打开应用 ev pipe 写端 */
    int ev_w = pipe_open(req->ev_name, 1);
    if (ev_w < 0) { w->used = 0; return NULL; }
    w->event_w = ev_w;

    mouse_restore();
    redraw_region(w->x, w->y, w->x + w->w + 4, w->y + w->h + 4);
    mouse_draw();
    return w;
}

static void handle_resize_request(struct wm_resize_request *req) {
    /* 用 owner_pid + old_shm 找窗口 */
    struct window *w = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].used) continue;
        if (g_wins[i].owner_pid != (int)req->owner_pid) continue;
        if (strcmp(g_wins[i].shm_name, req->old_shm) != 0) continue;
        w = &g_wins[i];
        break;
    }
    if (!w) return;
    if (req->new_w == 0 || req->new_h == 0) return;
    if (req->new_w > 1600 || req->new_h > 1200) return;

    /* 旧窗口包围盒（用于重绘并集） */
    int ox0, oy0, ox1, oy1;
    win_bbox(w, &ox0, &oy0, &ox1, &oy1);

    /* 打开新 shm */
    int cw = req->new_w;
    int ch = req->new_h;
    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(req->new_shm, shm_size, 0);
    if (shm_id < 0) return;

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) return;

    /* 清理旧 shm */
    if (!w->is_term) {
        /* 应用 shm——WM 侧的旧映射由 munmap 释放。
         * 注意：WM 侧没单独保存映射的虚拟地址，只有 w->shm 指针。
         * 这里简化：只 shm_unlink 旧名字（应用自己 munmap）。 */
    }
    /* 旧名字由应用 unlink（应用先 munmap 自己的旧映射） */
    /* 如果应用没 unlink，shm_unlink 在这里兜底——可能 ENOENT，忽略 */
    shm_unlink(req->old_shm);

    /* 更新窗口 */
    w->w = cw + 2 * BORDER;
    w->h = ch + TITLE_H + 2 * BORDER;
    strncpy(w->shm_name, req->new_shm, sizeof(w->shm_name) - 1);
    w->shm_idx = shm_id;
    w->shm = (struct shm_hdr*)addr;
    w->counter = &w->shm->counter;
    w->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    w->last_seen = 0;

    /* 位置夹紧 */
    if (w->x + w->w + 4 > (int)g_W) w->x = g_W - w->w - 4;
    if (w->y + w->h + 4 > (int)g_H) w->y = g_H - w->h - 4;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;

    /* 新包围盒 */
    int nx0, ny0, nx1, ny1;
    win_bbox(w, &nx0, &ny0, &nx1, &ny1);

    /* 并集重绘 */
    int ux0 = ox0 < nx0 ? ox0 : nx0;
    int uy0 = oy0 < ny0 ? oy0 : ny0;
    int ux1 = ox1 > nx1 ? ox1 : nx1;
    int uy1 = oy1 > ny1 ? oy1 : ny1;

    mouse_restore();
    redraw_region(ux0, uy0, ux1, uy1);
    mouse_draw();
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

    /* 删名字——term 已退出，读端已关闭 */
    char ev_name[24];
    snprintf(ev_name, sizeof(ev_name), "wm-term-%d", (int)(w - g_wins));
    pipe_unlink(ev_name);

    char shm_name[16];
    snprintf(shm_name, sizeof(shm_name), "term-%d", (int)(w - g_wins));
    shm_unlink(shm_name);

    w->used = 0;
    if (g_focus == w) g_focus = NULL;

    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
}

static void cleanup_dead_windows(void) {
    uint32_t pids[64];
    int n = proc_list(pids, 64);

    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *w = &g_wins[i];
        if (!w->used) continue;

        int alive = 0;
        for (int j = 0; j < n; j++) {
            if ((int)pids[j] == w->owner_pid) { alive = 1; break; }
        }
        if (alive) continue;

        int x0, y0, x1, y1;
        win_bbox(w, &x0, &y0, &x1, &y1);

        if (w->event_w >= 0) ipc_close(w->event_w);
        if (!w->is_term && w->shm_name[0])
            shm_unlink(w->shm_name);
        if (w->is_term && w->term_h >= 0) {
            kill(w->term_h, SIGKILL);
            int st; wait(w->term_h, &st, 200);
            process_close(w->term_h);
        }

        w->used = 0;
        if (g_focus == w) g_focus = NULL;

        mouse_restore();
        redraw_region(x0, y0, x1, y1);
        mouse_draw();
    }
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


    /* wm-inbox：先 unlink 清残留，再创建 */
    pipe_unlink(WM_INBOX_NAME);
    int inbox_fds[2];
    if (pipe_named(WM_INBOX_NAME, inbox_fds) < 0) {
        printf("wm: wm-inbox create failed\n");
        return 1;
    }
    g_inbox_rd = inbox_fds[0];
    ipc_close(inbox_fds[1]);   /* WM 只保留读端 */


    load_title_font();


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

    /* 拖动状态 */
    struct window *drag_win = NULL;
    int drag_dx = 0, drag_dy = 0;

    for (;;) {
        /* ---------- 1. 收集输入事件（合并本帧鼠标） ---------- */
        struct input_event ev;
        int have_last_mouse = 0;
        int last_mx = 0, last_my = 0;

        while (input_poll(&ev) == 0) {
            if (ev.type == INPUT_KEY) {
                if (g_focus && g_focus->event_w >= 0) {
                    char c = (char)ev.code;
                    pipe_write(g_focus->event_w, &c, 1);
                }
            } else if (ev.type == INPUT_MOUSE_MOVE) {
                last_mx = ev.x;
                last_my = ev.y;
                have_last_mouse = 1;
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
                    /* 释放 */
                    drag_win = NULL;
                }
            }
        }

        /* ---------- 2. 处理本帧鼠标位置（合并后） ---------- */
        if (have_last_mouse) {
            if (drag_win) {
                int nx = last_mx - drag_dx;
                int ny = last_my - drag_dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx + drag_win->w + 4 > (int)g_W)
                    nx = g_W - drag_win->w - 4;
                if (ny + drag_win->h + 4 > (int)g_H)
                    ny = g_H - drag_win->h - 4;

                if (nx != drag_win->x || ny != drag_win->y) {
                    mouse_restore();

                    int ox0 = drag_win->x;
                    int oy0 = drag_win->y;
                    int ox1 = ox0 + drag_win->w + 4;
                    int oy1 = oy0 + drag_win->h + 4;

                    drag_win->x = nx;
                    drag_win->y = ny;

                    int nx0 = drag_win->x;
                    int ny0 = drag_win->y;
                    int nx1 = nx0 + drag_win->w + 4;
                    int ny1 = ny0 + drag_win->h + 4;

                    int ux0 = ox0 < nx0 ? ox0 : nx0;
                    int uy0 = oy0 < ny0 ? oy0 : ny0;
                    int ux1 = ox1 > nx1 ? ox1 : nx1;
                    int uy1 = oy1 > ny1 ? oy1 : ny1;

                    redraw_region(ux0, uy0, ux1, uy1);
                    mouse_draw();
                }

                mouse_x = last_mx;
                mouse_y = last_my;
            } else {
                if (last_mx != mouse_x || last_my != mouse_y) {
                    mouse_restore();
                    mouse_x = last_mx;
                    mouse_y = last_my;
                    mouse_draw();
                }
            }
        }

        /* ---------- 3. wm-inbox 处理 ---------- */
        /* wm-inbox：处理请求（create / resize） */
        while (g_inbox_rd >= 0 &&
               pipe_avail(g_inbox_rd) >= 8) {   /* 最小头 */
            /* 先读 magic + type 判断 */
            uint32_t hdr[2];
            int n = pipe_read(g_inbox_rd, hdr, 8);
            if (n != 8) break;
            if (hdr[0] != WM_MAGIC) continue;

            if (hdr[1] == WM_REQ_CREATE) {
                struct wm_request req;
                req.magic = hdr[0];
                req.type  = hdr[1];
                int rem = sizeof(req) - 8;
                if (pipe_avail(g_inbox_rd) < rem) break;
                if (pipe_read(g_inbox_rd,
                              (char*)&req + 8, rem) != rem) break;
                make_app_window(&req);
            } else if (hdr[1] == WM_REQ_RESIZE) {
                struct wm_resize_request req;
                req.magic = hdr[0];
                req.type  = hdr[1];
                int rem = sizeof(req) - 8;
                if (pipe_avail(g_inbox_rd) < rem) break;
                if (pipe_read(g_inbox_rd,
                              (char*)&req + 8, rem) != rem) break;
                handle_resize_request(&req);
            } else {
                /* 未知类型——继续 */
                continue;
            }
        }

        /* ---------- 4. 窗口 shm 更新（拖动时跳过——已间接包含） ---------- */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used) continue;

            uint64_t c = *w->counter;

            if (drag_win) {
                /* 拖动时 redraw_region 每帧 blit shm，内容已最新 */
                w->last_seen = c;
                continue;
            }

            if (c == w->last_seen) continue;
            w->last_seen = c;

            if (!w->shm) continue;

            uint32_t dx0 = w->shm->pub_x0;
            uint32_t dy0 = w->shm->pub_y0;
            uint32_t dx1 = w->shm->pub_x1;
            uint32_t dy1 = w->shm->pub_y1;

            int cw = cli_w(w);
            int ch = cli_h(w);

            if (dx0 == SHM_DIRTY_EMPTY || dx1 <= dx0 || dy1 <= dy0) {
                dx0 = 0; dy0 = 0; dx1 = cw; dy1 = ch;
            }
            if (dx0 >= (uint32_t)cw || dy0 >= (uint32_t)ch) continue;
            if (dx1 > (uint32_t)cw) dx1 = cw;
            if (dy1 > (uint32_t)ch) dy1 = ch;
            if (dx1 <= dx0 || dy1 <= dy0) continue;

            int sx0 = cli_x(w) + dx0;
            int sy0 = cli_y(w) + dy0;
            int sx1 = cli_x(w) + dx1;
            int sy1 = cli_y(w) + dy1;

            int occluded = 0;
            for (int j = 0; j < MAX_WINDOWS; j++) {
                struct window *o = &g_wins[j];
                if (!o->used || o == w) continue;
                if (o->z <= w->z) continue;
                if (o->x >= sx1 || o->x + o->w <= sx0) continue;
                if (o->y >= sy1 || o->y + o->h <= sy0) continue;
                occluded = 1;
                break;
            }

            mouse_restore();

            if (occluded) {
                redraw_region(sx0, sy0, sx1, sy1);
            } else {
                for (uint32_t y = dy0; y < dy1; y++) {
                    unsigned int *dst = g_back + (cli_y(w) + y) * g_pitch_px
                                       + cli_x(w) + dx0;
                    unsigned int *src = w->pixels + y * cw + dx0;
                    memcpy(dst, src, (dx1 - dx0) * 4);
                }
                blit_back_to_fb(sx0, sy0, sx1, sy1);
            }

            mouse_draw();

            w->shm->pub_x0 = SHM_DIRTY_EMPTY;
            w->shm->pub_y0 = 0;
            w->shm->pub_x1 = 0;
            w->shm->pub_y1 = 0;
        }

        /* ---------- 5. 死窗口检测 ---------- */
        static int check_cnt = 0;
        if (++check_cnt >= 50) {
            check_cnt = 0;
            cleanup_dead_windows();
        }

        sleep_ms(2);
    }

    return 0;
}