/* wm — 窗口管理器（Desktop 版） */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>
#include "signal.h"
#include "wm_proto.h"
#include "wm_theme.h"
#include "wm_font.h"
#include "unicode_font.h"
#include "shm_layout.h"

static void mouse_restore(void);
static void mouse_draw(void);

/* ==================== 屏幕 ==================== */
static unsigned int *g_fb;
static unsigned int *g_back;
static unsigned int *g_scratch;                /* 半透明窗口离屏合成 */
static unsigned int  g_W, g_H, g_pitch_px;
static struct window *g_dragging = NULL;
static int g_no_present = 0;

static int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

static void set_clip(int x0, int y0, int x1, int y1) {
    g_clip_x0 = x0; g_clip_y0 = y0;
    g_clip_x1 = x1; g_clip_y1 = y1;
}

static void reset_clip(void) {
    g_clip_x0 = 0; g_clip_y0 = 0;
    g_clip_x1 = (int)g_W; g_clip_y1 = (int)g_H;
}

static void put_pixel_buf(unsigned int *buf, int x, int y, unsigned int rgb) {
    if (x < 0 || y < 0 || x >= (int)g_W || y >= (int)g_H) return;
    if (x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1) return;
    buf[y * g_pitch_px + x] = rgb;
}

static void fill_rect_buf(unsigned int *buf, int x, int y, int w, int h,
                           unsigned int rgb) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < g_clip_x0) x0 = g_clip_x0;
    if (y0 < g_clip_y0) y0 = g_clip_y0;
    if (x1 > g_clip_x1) x1 = g_clip_x1;
    if (y1 > g_clip_y1) y1 = g_clip_y1;
    if (x1 <= x0 || y1 <= y0) return;

    for (int j = y0; j < y1; j++) {
        unsigned int *row = buf + j * g_pitch_px;
        for (int i = x0; i < x1; i++)
            row[i] = rgb;
    }
}

static void blit_back_to_fb(int x0, int y0, int x1, int y1) {
    if (g_back == g_fb) return;

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

#define MAX_WINDOWS      8
#define CTRL_BTN_SIZE    16
#define CTRL_BTN_GAP     4
#define CTRL_BTN_MARGIN  4

struct window {
    int used;
    int kind;
    int x, y, w, h;
    char title[32];
    int z;

    int cw, ch;

    int owner_pid;
    char shm_name[16];

    int shm_idx;
    unsigned int *pixels;
    volatile uint64_t *counter;         /* 跨进程同步，必须 volatile */
    uint64_t last_seen;
    struct shm_hdr *shm;

    unsigned int map_addr;
    unsigned int map_size;

    int event_w;
    int minimized;
    int maximized;
    int restore_x, restore_y, restore_w, restore_h;
    uint32_t min_w, min_h, max_w, max_h;
    int closing;
    uint32_t close_tick;
    uint32_t cursor;
    uint32_t alpha;                     /* 0/255 = 不透明；否则按字节 alpha 混合 */
    int tab_seq;                        /* 同组 tab 顺序号，越小越靠左 */
};

static int g_inbox_rd = -1;
static int g_tab_seq = 1;
#define TAB_W 96
#define TAB_GAP 2
static struct window g_wins[MAX_WINDOWS];
static int g_next_z = 1;
static struct window *g_mouse_entered = NULL;

static struct window *g_mouse_down_win[3] = { NULL, NULL, NULL };
static uint32_t g_btn_state = 0;
static uint32_t g_mods = 0;

static struct window *g_focus = NULL;
static uint32_t g_wm_tick = 0;

/* ---------- 窗口坐标 ---------- */

static int cli_x(struct window *w) {
    return w->kind == WM_KIND_NORMAL ? w->x + BORDER : w->x;
}
static int cli_y(struct window *w) {
    return w->kind == WM_KIND_NORMAL ? w->y + TITLE_H : w->y;
}
static int cli_w(struct window *w) { return w->cw; }
static int cli_h(struct window *w) { return w->ch; }

/* ---------- 事件发送 ---------- */

static void send_event(struct window *w, uint32_t type, uint32_t code,
                       int x, int y, uint32_t extra) {
    if (!w || w->event_w < 0) return;

    if (type == WM_EV_MOUSE_MOVE && pipe_avail(w->event_w) > 512)
        return;

    struct wm_event ev;
    ev.type = type;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    ev.extra = extra;
    pipe_write(w->event_w, &ev, sizeof(ev));
}

/* ---------- 焦点 ---------- */

static void set_focus(struct window *w) {
    if (w && w->kind != WM_KIND_NORMAL) w = NULL;
    if (g_focus == w) return;
    if (g_focus && g_focus->kind == WM_KIND_NORMAL)
        send_event(g_focus, WM_EV_FOCUS_OUT, 0, 0, 0, 0);
    g_focus = w;
    if (w && w->kind == WM_KIND_NORMAL)
        send_event(w, WM_EV_FOCUS_IN, 0, 0, 0, 0);
}

/* ---------- 悬停进入/离开 ---------- */

static void update_mouse_entered(struct window *target, int mx, int my) {
    if (target == g_mouse_entered) return;
    if (g_mouse_entered)
        send_event(g_mouse_entered, WM_EV_MOUSE_LEAVE, 0, 0, 0, 0);
    g_mouse_entered = target;
    if (target) {
        int cx = mx - cli_x(target);
        int cy = my - cli_y(target);
        send_event(target, WM_EV_MOUSE_ENTER, 0, cx, cy, g_mods);
    }
}

/* ---------- 窗口内容 blit ---------- */

static void shm_clear_pub(struct window *w) {
    if (!w || !w->shm) return;
    w->shm->pub_x0 = SHM_DIRTY_EMPTY;
    w->shm->pub_y0 = 0;
    w->shm->pub_x1 = 0;
    w->shm->pub_y1 = 0;
}

static void blit_shm_to_back(struct window *w, int rx0, int ry0, int rx1, int ry1) {
    int cw = cli_w(w);
    int ch = cli_h(w);
    int cx = cli_x(w);
    int cy = cli_y(w);

    int vis_w = cw;
    int vis_h = ch;
    if (w->kind == WM_KIND_NORMAL) {
        int max_w = w->w - 2 * BORDER;
        int max_h = w->h - TITLE_H - 2 * BORDER;
        if (vis_w > max_w) vis_w = max_w;
        if (vis_h > max_h) vis_h = max_h;
    } else {
        if (vis_w > w->w) vis_w = w->w;
        if (vis_h > w->h) vis_h = w->h;
    }
    if (vis_w <= 0 || vis_h <= 0) return;

    int x0 = cx;
    int x1 = cx + vis_w;
    int y0 = cy;
    int y1 = cy + vis_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_W) x1 = g_W;
    if (y1 > (int)g_H) y1 = g_H;

    if (x0 < rx0) x0 = rx0;
    if (y0 < ry0) y0 = ry0;
    if (x1 > rx1) x1 = rx1;
    if (y1 > ry1) y1 = ry1;
    if (x1 <= x0 || y1 <= y0) return;

    int sx = x0 - cx;
    int sy = y0 - cy;
    /* NORMAL 窗口的半透明由 redraw_region 外围的 scratch 处理，
     * 这里只对 DOCK/POPUP 做逐像素 alpha 混合。 */
    int blend = (w->kind != WM_KIND_NORMAL && w->alpha > 0 && w->alpha < 255);
    unsigned int a = w->alpha;

    for (int y = y0; y < y1; y++) {
        unsigned int *dst = g_back + y * g_pitch_px + x0;
        unsigned int *src = w->pixels + (sy + (y - y0)) * cw + sx;
        if (!blend) {
            memcpy(dst, src, (x1 - x0) * 4);
        } else {
            for (int x = 0; x < x1 - x0; x++) {
                unsigned int c = src[x];
                unsigned int d = dst[x];
                unsigned int r = (((c >> 16) & 255) * a + ((d >> 16) & 255) * (255 - a)) / 255;
                unsigned int g = (((c >> 8) & 255) * a + ((d >> 8) & 255) * (255 - a)) / 255;
                unsigned int b = ((c & 255) * a + (d & 255) * (255 - a)) / 255;
                dst[x] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void alpha_blit_rect(unsigned int *src, unsigned int *dst, int stride,
                            int x0, int y0, int x1, int y1, unsigned int a) {
    for (int y = y0; y < y1; y++) {
        unsigned int *s = src + y * stride + x0;
        unsigned int *d = dst + y * stride + x0;
        for (int x = x0; x < x1; x++) {
            unsigned int c = *s++;
            unsigned int bg = *d;
            unsigned int r = (((c >> 16) & 255) * a + ((bg >> 16) & 255) * (255 - a)) / 255;
            unsigned int g = (((c >> 8) & 255) * a + ((bg >> 8) & 255) * (255 - a)) / 255;
            unsigned int b = ((c & 255) * a + (bg & 255) * (255 - a)) / 255;
            *d++ = (r << 16) | (g << 8) | b;
        }
    }
}

static void draw_title_text(struct window *w);

/* ---------- 三按钮 ---------- */

static void draw_ctrl_minimize(int bx, int by) {
    fill_rect_buf(g_back, bx, by, CTRL_BTN_SIZE, CTRL_BTN_SIZE, COL_BORDER);
    for (int i = 0; i < 8; i++)
        put_pixel_buf(g_back, bx + 4 + i, by + 8, 0x000000);
}

static void draw_ctrl_maximize(int bx, int by) {
    fill_rect_buf(g_back, bx, by, CTRL_BTN_SIZE, CTRL_BTN_SIZE, COL_BORDER);
    for (int i = 0; i < 8; i++) {
        put_pixel_buf(g_back, bx + 4 + i, by + 4, 0x000000);
        put_pixel_buf(g_back, bx + 4 + i, by + 12, 0x000000);
        put_pixel_buf(g_back, bx + 4, by + 4 + i, 0x000000);
        put_pixel_buf(g_back, bx + 12, by + 4 + i, 0x000000);
    }
}

static void draw_ctrl_close(int bx, int by, unsigned int color) {
    fill_rect_buf(g_back, bx, by, CTRL_BTN_SIZE, CTRL_BTN_SIZE, color);
    for (int i = 0; i < 8; i++) {
        put_pixel_buf(g_back, bx + 4 + i, by + 4 + i, 0xFFFFFF);
        put_pixel_buf(g_back, bx + 12 - i, by + 4 + i, 0xFFFFFF);
    }
}

/* ---------- tab 分组 ---------- */

static int tab_group_of(struct window *w) {
    if (!w || w->kind != WM_KIND_NORMAL) return 0;
    if (strncmp(w->title, "Files", 5) == 0 || strncmp(w->title, "File", 4) == 0) return 1;
    if (strncmp(w->title, "Terminal", 8) == 0) return 2;
    return 0;
}

static int tab_count(int group) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (g_wins[i].used && tab_group_of(&g_wins[i]) == group) n++;
    return n;
}

static struct window *tab_nth(int group, int n) {
    for (int seq = 1; seq < g_tab_seq; seq++)
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (w->used && tab_group_of(w) == group && w->tab_seq == seq) {
                if (n-- == 0) return w;
            }
        }
    return NULL;
}

/* 第 n 个 tab 的横向起点；超出 limit 后返回 limit（用于命中测试与绘制一致） */
static int tab_x_at(int start, int idx) {
    return start + idx * (TAB_W + TAB_GAP);
}

/* 计算在 [start, limit) 内能放下的 tab 数 */
static int tab_fit_count(int start, int limit) {
    int n = 0;
    while (tab_x_at(start, n) + TAB_W <= limit) n++;
    return n;
}

/* ---------- 窗口装饰 ---------- */

static void draw_tab_text(int x, int y, int limit, const char *s) {
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp;
        int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = FONT_W;
        if (x + gw > limit) break;
        if (g) for (int r = 0; r < FONT_H; r++) for (int j = 0; j < gw; j++) {
            int bit = gw == 16 ? (g[r*2 + (j>>3)] & (0x80 >> (j&7)))
                               : (g[r] & (0x80 >> j));
            if (bit) put_pixel_buf(g_back, x + j, y + r, 0xFFFFFF);
        }
        x += gw; p += n;
    }
}

static void draw_tab_bar(struct window *w) {
    if (!wm_font_loaded()) return;
    int group = tab_group_of(w);
    int start = w->x + 4;
    int limit = w->x + w->w - CTRL_BTN_MARGIN - CTRL_BTN_SIZE * 3 - CTRL_BTN_GAP * 2 - 4;
    int ty = w->y + 2;
    int total = tab_count(group);
    int fit = tab_fit_count(start, limit);
    int n = (fit < total) ? fit : total;

    for (int i = 0; i < n; i++) {
        struct window *t = tab_nth(group, i);
        if (!t) break;
        int x = tab_x_at(start, i);
        fill_rect_buf(g_back, x, ty, TAB_W, TITLE_H - 4,
                      t == w ? 0x3A6EA5 : 0x2A3A50);
        char label[40];
        if (group == 2) snprintf(label, sizeof(label), "Term %d", i + 1);
        else snprintf(label, sizeof(label), "%s", t->title);
        draw_tab_text(x + 4, w->y + (TITLE_H - FONT_H) / 2,
                      x + TAB_W - 16, label);
        for (int k = 0; k < 10; k++) {
            put_pixel_buf(g_back, x + TAB_W - 12 + k, w->y + 6 + k, 0xE0E0E0);
            put_pixel_buf(g_back, x + TAB_W - 2 - k, w->y + 6 + k, 0xE0E0E0);
        }
    }

    int px = tab_x_at(start, n);
    if (px + 18 <= limit) {
        fill_rect_buf(g_back, px, ty, 18, TITLE_H - 4, 0x2A3A50);
        fill_rect_buf(g_back, px + 8, w->y + 6, 2, 12, 0xFFFFFF);
        fill_rect_buf(g_back, px + 3, w->y + 11, 12, 2, 0xFFFFFF);
    }
}

static void render_win_frame(struct window *w) {
    if (w->kind != WM_KIND_NORMAL) return;

    fill_rect_buf(g_back, w->x + 4, w->y + 4, w->w, w->h, 0x000000);
    fill_rect_buf(g_back, w->x, w->y, w->w, TITLE_H, COL_TITLE);
    fill_rect_buf(g_back, w->x, w->y, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y + w->h - BORDER, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, w->x + w->w - BORDER, w->y, BORDER, w->h, COL_BORDER);

    int actual_cw = w->w - 2 * BORDER;
    int actual_ch = w->h - TITLE_H - 2 * BORDER;
    fill_rect_buf(g_back, cli_x(w), cli_y(w), actual_cw, actual_ch, 0x000000);

    if (tab_group_of(w)) draw_tab_bar(w);
    else draw_title_text(w);

    int by = w->y + (TITLE_H - CTRL_BTN_SIZE) / 2;
    int bx_close = w->x + w->w - CTRL_BTN_MARGIN - CTRL_BTN_SIZE;
    int bx_max   = bx_close - CTRL_BTN_GAP - CTRL_BTN_SIZE;
    int bx_min   = bx_max   - CTRL_BTN_GAP - CTRL_BTN_SIZE;

    unsigned int close_col = (w == g_focus) ? COL_CLOSE_HL : COL_CLOSE;

    draw_ctrl_minimize(bx_min, by);
    draw_ctrl_maximize(bx_max, by);
    draw_ctrl_close(bx_close, by, close_col);
}

static void draw_title_text(struct window *w) {
    if (!wm_font_loaded()) return;

    int tx = w->x + 8;
    int ty = w->y + (TITLE_H - FONT_H) / 2;
    int tx_limit = w->x + w->w - CTRL_BTN_MARGIN - CTRL_BTN_SIZE * 3 - CTRL_BTN_GAP * 2 - 8;

    const unsigned char *p = (const unsigned char*)w->title;
    while (*p) {
        uint32_t cp;
        int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = FONT_W;
        if (tx + gw > tx_limit) break;
        if (g) {
            for (int r = 0; r < FONT_H; r++) {
                for (int j = 0; j < gw; j++) {
                    int bit = gw == 16 ? (g[r*2 + (j>>3)] & (0x80 >> (j&7)))
                                       : (g[r] & (0x80 >> j));
                    if (bit) put_pixel_buf(g_back, tx + j, ty + r, COL_TITLE_FG);
                }
            }
        }
        tx += gw;
        p += n;
    }
}

/* ---------- 分层 ---------- */

static int layer_of(struct window *w) {
    switch (w->kind) {
        case WM_KIND_WALLPAPER: return 0;
        case WM_KIND_NORMAL:    return 1;
        case WM_KIND_DOCK:      return 2;
        case WM_KIND_POPUP:     return 3;
        default: return 1;
    }
}

static void redraw_region(int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)g_W) x1 = (int)g_W;
    if (y1 > (int)g_H) y1 = (int)g_H;
    if (x1 <= x0 || y1 <= y0) return;

    set_clip(x0, y0, x1, y1);

    struct window *wall = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].kind == WM_KIND_WALLPAPER) {
            wall = &g_wins[i];
            break;
        }
    }
    if (wall) {
        int cw = wall->cw;
        for (int y = y0; y < y1; y++) {
            unsigned int *dst = g_back + y * g_pitch_px + x0;
            unsigned int *src = wall->pixels + y * cw + x0;
            memcpy(dst, src, (x1 - x0) * 4);
        }
    } else {
        fill_rect_buf(g_back, x0, y0, x1 - x0, y1 - y0, COL_BG);
    }

    for (int layer = 1; layer <= 3; layer++) {
        for (int z = 0; z <= g_next_z; z++) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                struct window *w = &g_wins[i];
                if (!w->used || w->minimized) continue;
                if (w == g_dragging) continue;
                if (layer_of(w) != layer) continue;
                if (w->z != z) continue;
                if (layer == 1) {
                    if (w->x + 4 >= x1 || w->x + w->w + 4 <= x0) continue;
                    if (w->y + 4 >= y1 || w->y + w->h + 4 <= y0) continue;
                    if (w->alpha > 0 && w->alpha < 255 && g_scratch) {
                        unsigned int *real_back = g_back;
                        int bx0 = w->x, by0 = w->y;
                        int bx1 = w->x + w->w + 4, by1 = w->y + w->h + 4;
                        if (bx0 < x0) bx0 = x0;
                        if (by0 < y0) by0 = y0;
                        if (bx1 > x1) bx1 = x1;
                        if (by1 > y1) by1 = y1;
                        for (int yy = by0; yy < by1; yy++)
                            memcpy(g_scratch + yy * g_pitch_px + bx0,
                                   real_back + yy * g_pitch_px + bx0,
                                   (bx1 - bx0) * 4);
                        g_back = g_scratch;
                        render_win_frame(w);
                        blit_shm_to_back(w, x0, y0, x1, y1);
                        g_back = real_back;
                        alpha_blit_rect(g_scratch, real_back, g_pitch_px,
                                        bx0, by0, bx1, by1, w->alpha);
                        continue;
                    }
                    render_win_frame(w);
                } else {
                    if (w->x >= x1 || w->x + w->w <= x0) continue;
                    if (w->y >= y1 || w->y + w->h <= y0) continue;
                }
                blit_shm_to_back(w, x0, y0, x1, y1);
            }
        }
    }

    reset_clip();
    if (!g_no_present) blit_back_to_fb(x0, y0, x1, y1);
}

static void win_bbox(struct window *w, int *x0, int *y0, int *x1, int *y1) {
    if (w->kind == WM_KIND_NORMAL) {
        *x0 = w->x;
        *y0 = w->y;
        *x1 = w->x + w->w + 4;
        *y1 = w->y + w->h + 4;
    } else {
        *x0 = w->x;
        *y0 = w->y;
        *x1 = w->x + w->w;
        *y1 = w->y + w->h;
    }
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
static unsigned int mouse_save[CUR_W * CUR_H];

static int mouse_intersects_rect(int x0, int y0, int x1, int y1) {
    return x1 > mouse_x && x0 < mouse_x + CUR_W &&
           y1 > mouse_y && y0 < mouse_y + CUR_H;
}

/* 软件光标：在 VRAM 前缓冲直接读-改-写。
 * 合成前 mouse_restore 把上次覆盖的像素写回；
 * 合成后 mouse_draw 把光标重新叠上去。 */
static void mouse_restore(void) {
    if (mouse_prev_x < 0) return;
    for (int j = 0; j < CUR_H; j++) {
        int py = mouse_prev_y + j;
        if (py < 0 || py >= (int)g_H) continue;
        for (int i = 0; i < CUR_W; i++) {
            int px = mouse_prev_x + i;
            if (px < 0 || px >= (int)g_W) continue;
            g_fb[py * g_pitch_px + px] = mouse_save[j * CUR_W + i];
        }
    }
    mouse_prev_x = -1;
    mouse_prev_y = -1;
}

static void mouse_draw(void) {
    for (int j = 0; j < CUR_H; j++) {
        unsigned short row = cursor_bits[j];
        int py = mouse_y + j;
        if (py < 0 || py >= (int)g_H) continue;
        for (int i = 0; i < CUR_W; i++) {
            int px = mouse_x + i;
            if (px < 0 || px >= (int)g_W) continue;
            int off = py * g_pitch_px + px;
            mouse_save[j * CUR_W + i] = g_fb[off];
            if (row & (0x8000 >> i)) g_fb[off] = 0xFFFFFF;
        }
    }
    mouse_prev_x = mouse_x;
    mouse_prev_y = mouse_y;
}

static struct window *find_by_shm(const char *shm_name) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && strcmp(g_wins[i].shm_name, shm_name) == 0)
            return &g_wins[i];
    }
    return NULL;
}

static struct window *find_by_kind(int kind) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].kind == kind) return &g_wins[i];
    }
    return NULL;
}

static int dock_height(void) {
    struct window *dock = find_by_kind(WM_KIND_DOCK);
    return dock ? dock->h : 0;
}

/* ==================== 窗口列表推送 ==================== */

static void push_window_list(void) {
    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (!dock || dock->event_w < 0) return;

    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].kind == WM_KIND_NORMAL) n++;
    }
    if (n > MAX_WINDOWS) n = MAX_WINDOWS;

    uint8_t buf[sizeof(struct wm_event) + MAX_WINDOWS * sizeof(struct wm_window_entry)];
    struct wm_event *ev = (struct wm_event*)buf;
    ev->type  = WM_EV_WINDOW_LIST;
    ev->code  = (uint32_t)n;
    ev->x = 0;
    ev->y = 0;
    ev->extra = 0;

    int off = sizeof(struct wm_event);
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS && count < n; i++) {
        struct window *w = &g_wins[i];
        if (!w->used || w->kind != WM_KIND_NORMAL) continue;
        struct wm_window_entry *e = (struct wm_window_entry*)(buf + off);
        e->id = (uint32_t)i;
        e->minimized = (uint32_t)w->minimized;
        memset(e->title, 0, 32);
        strncpy(e->title, w->title, 31);
        off += sizeof(struct wm_window_entry);
        count++;
    }

    pipe_write(dock->event_w, buf, off);
}

/* ==================== 窗口创建/销毁 ==================== */

static void activate_tab(struct window *w) {
    int g = tab_group_of(w);
    if (!g) { set_focus(w); return; }
    struct window *old = g_focus;
    int ox = (old && tab_group_of(old) == g) ? old->x : w->x;
    int oy = (old && tab_group_of(old) == g) ? old->y : w->y;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *o = &g_wins[i];
        if (!o->used || tab_group_of(o) != g) continue;
        if (o == w) {
            o->minimized = 0;
            o->x = ox; o->y = oy;
            o->z = g_next_z++;
        } else {
            o->minimized = 1;
        }
    }
    set_focus(w);
    mouse_restore();
    redraw_region(0, 0, g_W, g_H);
    mouse_draw();
    push_window_list();
}

static int tab_hit_test(struct window *w, int mx, int my,
                        struct window **out, int *close_hit, int *plus_hit) {
    if (!w || !tab_group_of(w)) return 0;
    if (my < w->y || my >= w->y + TITLE_H) return 0;
    int group = tab_group_of(w);
    int start = w->x + 4;
    int limit = w->x + w->w - CTRL_BTN_MARGIN - CTRL_BTN_SIZE * 3 - CTRL_BTN_GAP * 2 - 4;
    int total = tab_count(group);
    int fit = tab_fit_count(start, limit);
    int n = (fit < total) ? fit : total;

    for (int i = 0; i < n; i++) {
        int x = tab_x_at(start, i);
        if (mx >= x && mx < x + TAB_W) {
            *out = tab_nth(group, i);
            *close_hit = mx >= x + TAB_W - 14;
            *plus_hit = 0;
            return *out != NULL;
        }
    }
    int px = tab_x_at(start, n);
    if (px + 18 <= limit && mx >= px && mx < px + 18) {
        *out = w; *close_hit = 0; *plus_hit = 1;
        return 1;
    }
    return 0;
}

static struct window *make_app_window(struct wm_request *req) {
    if (req->w == 0 || req->h == 0) return NULL;
    if (req->w > 2048 || req->h > 1536) return NULL;
    if (req->kind > WM_KIND_WALLPAPER) return NULL;

    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!g_wins[i].used) { slot = i; break; }
    if (slot < 0) return NULL;

    struct window *w = &g_wins[slot];
    memset(w, 0, sizeof(*w));
    w->used = 1;
    w->kind = req->kind;
    w->cw = req->w;
    w->ch = req->h;
    w->owner_pid = req->owner_pid;
    w->minimized = 0;
    w->maximized = 0;
    w->closing = 0;
    w->cursor = WM_CURSOR_ARROW;
    w->min_w = req->min_w;
    w->min_h = req->min_h;
    w->max_w = req->max_w;
    w->max_h = req->max_h;
    w->alpha = 255;
    strncpy(w->title, req->title, sizeof(w->title) - 1);
    strncpy(w->shm_name, req->shm_name, sizeof(w->shm_name) - 1);
    w->tab_seq = g_tab_seq++;

    if (req->kind == WM_KIND_NORMAL) {
        w->w = req->w + 2 * BORDER;
        w->h = req->h + TITLE_H + 2 * BORDER;
        w->x = req->x ? (int)req->x : 120 + slot * 40;
        w->y = req->y ? (int)req->y : 120 + slot * 40;
    } else if (req->kind == WM_KIND_DOCK) {
        w->w = req->w;
        w->h = req->h;
        w->x = (int)req->x;
        w->y = req->y ? (int)req->y : (int)g_H - (int)req->h;
    } else if (req->kind == WM_KIND_POPUP) {
        w->w = req->w;
        w->h = req->h;
        w->x = (int)req->x;
        w->y = (int)req->y;
    } else {
        w->w = req->w;
        w->h = req->h;
        w->x = 0;
        w->y = 0;
    }
    w->z = g_next_z++;

    int shm_size = shm_total_size(req->w, req->h);
    int shm_id = shm_open(req->shm_name, shm_size, 0);
    if (shm_id < 0) { w->used = 0; return NULL; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) { w->used = 0; return NULL; }

    w->shm_idx = shm_id;
    w->shm = (struct shm_hdr*)addr;
    w->map_addr = addr;
    w->map_size = shm_size;
    w->counter = &w->shm->counter;
    w->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    w->last_seen = (*w->counter == 0) ? 0 : *w->counter - 1;

    int ev_w = pipe_open(req->ev_name, 1);
    if (ev_w < 0) { w->used = 0; return NULL; }
    w->event_w = ev_w;

    mouse_restore();
    if (w->kind == WM_KIND_NORMAL) {
        render_win_frame(w);
        int bx0, by0, bx1, by1;
        win_bbox(w, &bx0, &by0, &bx1, &by1);
        blit_back_to_fb(bx0, by0, bx1, by1);
    }
    mouse_draw();
    if (w->kind == WM_KIND_NORMAL) set_focus(w);
    push_window_list();
    return w;
}

static void forget_window_buttons(struct window *w) {
    for (int bi = 0; bi < 3; bi++) {
        if (g_mouse_down_win[bi] == w) g_mouse_down_win[bi] = NULL;
    }
}

static void minimize_window(struct window *w) {
    if (!w || w->minimized) return;
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);
    w->minimized = 1;
    if (g_focus == w) set_focus(NULL);
    if (g_mouse_entered == w) g_mouse_entered = NULL;
    forget_window_buttons(w);
    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void restore_window(struct window *w) {
    if (!w || !w->minimized) return;
    w->minimized = 0;
    w->z = g_next_z++;
    shm_clear_pub(w);
    set_focus(w);
    mouse_restore();
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void maximize_window(struct window *w) {
    if (!w || w->kind != WM_KIND_NORMAL || w->maximized) return;

    int ox0, oy0, ox1, oy1;
    win_bbox(w, &ox0, &oy0, &ox1, &oy1);

    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->w;
    w->restore_h = w->h;

    w->x = 0;
    w->y = 0;
    w->w = g_W;
    w->h = g_H - dock_height();
    w->maximized = 1;

    int new_cw = w->w - 2 * BORDER;
    int new_ch = w->h - TITLE_H - 2 * BORDER;
    send_event(w, WM_EV_RESIZE, (uint32_t)new_cw, 0, 0, (uint32_t)new_ch);

    int nx0, ny0, nx1, ny1;
    win_bbox(w, &nx0, &ny0, &nx1, &ny1);
    int ux0 = ox0 < nx0 ? ox0 : nx0;
    int uy0 = oy0 < ny0 ? oy0 : ny0;
    int ux1 = ox1 > nx1 ? ox1 : nx1;
    int uy1 = oy1 > ny1 ? oy1 : ny1;

    mouse_restore();
    redraw_region(ux0, uy0, ux1, uy1);
    mouse_draw();
    push_window_list();
}

static void unmaximize_window(struct window *w) {
    if (!w || !w->maximized) return;

    int ox0, oy0, ox1, oy1;
    win_bbox(w, &ox0, &oy0, &ox1, &oy1);

    w->x = w->restore_x;
    w->y = w->restore_y;
    w->w = w->restore_w;
    w->h = w->restore_h;
    w->maximized = 0;

    int new_cw = w->w - 2 * BORDER;
    int new_ch = w->h - TITLE_H - 2 * BORDER;
    send_event(w, WM_EV_RESIZE, (uint32_t)new_cw, 0, 0, (uint32_t)new_ch);

    int nx0, ny0, nx1, ny1;
    win_bbox(w, &nx0, &ny0, &nx1, &ny1);
    int ux0 = ox0 < nx0 ? ox0 : nx0;
    int uy0 = oy0 < ny0 ? oy0 : ny0;
    int ux1 = ox1 > nx1 ? ox1 : nx1;
    int uy1 = oy1 > ny1 ? oy1 : ny1;

    mouse_restore();
    redraw_region(ux0, uy0, ux1, uy1);
    mouse_draw();
    push_window_list();
}

static void destroy_window(struct window *w) {
    if (!w || !w->used) return;

    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);

    if (w->kind == WM_KIND_POPUP) {
        send_event(w, WM_EV_CLOSE, 0, 0, 0, 0);
    }

    if (w->event_w >= 0) ipc_close(w->event_w);

    if (w->map_addr) {
        munmap(w->map_addr, w->map_size);
        w->map_addr = 0;
        w->map_size = 0;
    }
    if (w->shm_name[0]) shm_unlink(w->shm_name);

    int tab_group = tab_group_of(w);
    w->used = 0;
    w->event_w = -1;
    if (g_focus == w) set_focus(NULL);
    if (g_mouse_entered == w) g_mouse_entered = NULL;
    if (g_dragging == w) g_dragging = NULL;
    if (tab_group) {
        struct window *next = tab_nth(tab_group, 0);
        if (next) {
            next->minimized = 0;
            next->x = x0; next->y = y0;
            next->z = g_next_z++;
            set_focus(next);
        }
    }
    forget_window_buttons(w);

    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void handle_resize_request(struct wm_resize_request *req) {
    struct window *w = find_by_shm(req->old_shm);
    if (!w) return;

    int cw = (int)req->new_w;
    int ch = (int)req->new_h;

    if (w->min_w && cw < (int)w->min_w) cw = (int)w->min_w;
    if (w->min_h && ch < (int)w->min_h) ch = (int)w->min_h;
    if (w->max_w && cw > (int)w->max_w) cw = (int)w->max_w;
    if (w->max_h && ch > (int)w->max_h) ch = (int)w->max_h;

    if (cw == 0 || ch == 0) return;
    if (cw > 2048 || ch > 1536) return;

    int ox0, oy0, ox1, oy1;
    win_bbox(w, &ox0, &oy0, &ox1, &oy1);

    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(req->new_shm, shm_size, 0);
    if (shm_id < 0) return;

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) return;

    if (w->map_addr) {
        munmap(w->map_addr, w->map_size);
        w->map_addr = 0;
        w->map_size = 0;
    }

    shm_unlink(req->old_shm);

    w->cw = cw;
    w->ch = ch;
    if (w->kind == WM_KIND_NORMAL) {
        w->w = cw + 2 * BORDER;
        w->h = ch + TITLE_H + 2 * BORDER;
    } else {
        w->w = cw;
        w->h = ch;
    }
    strncpy(w->shm_name, req->new_shm, sizeof(w->shm_name) - 1);
    w->shm_idx = shm_id;
    w->shm = (struct shm_hdr*)addr;
    w->counter = &w->shm->counter;
    w->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    w->map_addr = addr;
    w->map_size = shm_size;
    w->last_seen = 0;

    if (w->x + w->w + 4 > (int)g_W) w->x = g_W - w->w - 4;
    if (w->y + w->h + 4 > (int)g_H) w->y = g_H - w->h - 4;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;

    int nx0, ny0, nx1, ny1;
    win_bbox(w, &nx0, &ny0, &nx1, &ny1);

    int ux0 = ox0 < nx0 ? ox0 : nx0;
    int uy0 = oy0 < ny0 ? oy0 : ny0;
    int ux1 = ox1 > nx1 ? ox1 : nx1;
    int uy1 = oy1 > ny1 ? oy1 : ny1;

    mouse_restore();
    redraw_region(ux0, uy0, ux1, uy1);
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

        destroy_window(w);
    }
}

/* ==================== 输入路由 ==================== */

static void route_mouse_move(int mx, int my) {
    struct window *target = NULL;

    struct window *popup = find_by_kind(WM_KIND_POPUP);
    if (popup && mx >= popup->x && mx < popup->x + popup->w &&
        my >= popup->y && my < popup->y + popup->h) {
        target = popup;
    }

    if (!target) {
        struct window *dock = find_by_kind(WM_KIND_DOCK);
        if (dock && mx >= dock->x && mx < dock->x + dock->w &&
            my >= dock->y && my < dock->y + dock->h) {
            target = dock;
        }
    }

    if (!target) {
        int best_z = -1;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used || w->minimized || w->kind != WM_KIND_NORMAL) continue;
            if (mx >= w->x && mx < w->x + w->w &&
                my >= w->y && my < w->y + w->h) {
                if (w->z > best_z) { best_z = w->z; target = w; }
            }
        }
    }

    if (!target) {
        struct window *wall = find_by_kind(WM_KIND_WALLPAPER);
        if (wall) target = wall;
    }

    update_mouse_entered(target, mx, my);

    if (target) {
        int cx = mx - cli_x(target);
        int cy = my - cli_y(target);
        send_event(target, WM_EV_MOUSE_MOVE, g_btn_state, cx, cy, g_mods);
    }
}

static void handle_mouse_press(int mx, int my,
                                struct window **drag_win,
                                int *drag_dx, int *drag_dy) {
    struct window *popup = find_by_kind(WM_KIND_POPUP);
    if (popup) {
        if (mx >= popup->x && mx < popup->x + popup->w &&
            my >= popup->y && my < popup->y + popup->h) {
            int cx = mx - cli_x(popup);
            int cy = my - cli_y(popup);
            g_mouse_down_win[0] = popup;
            send_event(popup, WM_EV_MOUSE_DOWN, WM_BTN_LEFT, cx, cy, g_mods);
            return;
        }
        destroy_window(popup);
    }

    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (dock && mx >= dock->x && mx < dock->x + dock->w &&
        my >= dock->y && my < dock->y + dock->h) {
        int cx = mx - cli_x(dock);
        int cy = my - cli_y(dock);
        g_mouse_down_win[0] = dock;
        send_event(dock, WM_EV_MOUSE_DOWN, WM_BTN_LEFT, cx, cy, g_mods);
        return;
    }

    struct window *hit = NULL;
    int best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *w = &g_wins[i];
        if (!w->used || w->minimized || w->kind != WM_KIND_NORMAL) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h) {
            if (w->z > best_z) { best_z = w->z; hit = w; }
        }
    }

    if (hit) {
        int need_full = (hit != g_focus);
        set_focus(hit);
        hit->z = g_next_z++;

        int by       = hit->y + (TITLE_H - CTRL_BTN_SIZE) / 2;
        int bx_close = hit->x + hit->w - CTRL_BTN_MARGIN - CTRL_BTN_SIZE;
        int bx_max   = bx_close - CTRL_BTN_GAP - CTRL_BTN_SIZE;
        int bx_min   = bx_max   - CTRL_BTN_GAP - CTRL_BTN_SIZE;

        if (my >= by && my < by + CTRL_BTN_SIZE) {
            if (mx >= bx_close && mx < bx_close + CTRL_BTN_SIZE) {
                send_event(hit, WM_EV_CLOSE_REQUEST, 0, 0, 0, 0);
                hit->closing = 1;
                hit->close_tick = g_wm_tick;
                return;
            }
            if (mx >= bx_max && mx < bx_max + CTRL_BTN_SIZE) {
                if (hit->maximized) unmaximize_window(hit);
                else                maximize_window(hit);
                if (need_full) {
                    mouse_restore();
                    redraw_region(0, 0, g_W, g_H);
                    mouse_draw();
                }
                return;
            }
            if (mx >= bx_min && mx < bx_min + CTRL_BTN_SIZE) {
                minimize_window(hit);
                return;
            }
        }

        if (my >= hit->y && my < hit->y + TITLE_H && tab_group_of(hit)) {
            struct window *clicked = NULL;
            int close_hit = 0, plus_hit = 0;
            if (tab_hit_test(hit, mx, my, &clicked, &close_hit, &plus_hit)) {
                if (plus_hit) {
                    send_event(hit, WM_EV_NEW_TAB, 0, 0, 0, 0);
                } else if (clicked && close_hit) {
                    send_event(clicked, WM_EV_CLOSE_REQUEST, 0, 0, 0, 0);
                    clicked->closing = 1;
                    clicked->close_tick = g_wm_tick;
                } else if (clicked) {
                    activate_tab(clicked);
                }
                return;
            }
        }

        if (my >= hit->y && my < hit->y + TITLE_H) {
            if (!hit->maximized) {
                *drag_win = hit;
                *drag_dx = mx - hit->x;
                *drag_dy = my - hit->y;
            }
            if (need_full) {
                mouse_restore();
                redraw_region(0, 0, g_W, g_H);
                mouse_draw();
            }
            return;
        }

        int cx = mx - cli_x(hit);
        int cy = my - cli_y(hit);
        if (cx >= 0 && cx < hit->cw && cy >= 0 && cy < hit->ch) {
            g_mouse_down_win[0] = hit;
            send_event(hit, WM_EV_MOUSE_DOWN, WM_BTN_LEFT, cx, cy, g_mods);
        }
        if (need_full) {
            mouse_restore();
            redraw_region(0, 0, g_W, g_H);
            mouse_draw();
        }
        return;
    }

    struct window *wall = find_by_kind(WM_KIND_WALLPAPER);
    if (wall) {
        set_focus(NULL);
        int cx = mx - cli_x(wall);
        int cy = my - cli_y(wall);
        g_mouse_down_win[0] = wall;
        send_event(wall, WM_EV_MOUSE_DOWN, WM_BTN_LEFT, cx, cy, g_mods);
    }
}

static struct window *find_top_normal_window(int mx, int my) {
    struct window *hit = NULL;
    int best_z = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        struct window *w = &g_wins[i];
        if (!w->used || w->minimized || w->kind != WM_KIND_NORMAL) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h) {
            if (w->z > best_z) { best_z = w->z; hit = w; }
        }
    }
    return hit;
}

static struct window *window_at(int mx, int my) {
    struct window *popup = find_by_kind(WM_KIND_POPUP);
    if (popup && mx >= popup->x && mx < popup->x + popup->w &&
        my >= popup->y && my < popup->y + popup->h)
        return popup;

    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (dock && mx >= dock->x && mx < dock->x + dock->w &&
        my >= dock->y && my < dock->y + dock->h)
        return dock;

    struct window *n = find_top_normal_window(mx, my);
    if (n) return n;

    return find_by_kind(WM_KIND_WALLPAPER);
}

static void handle_other_button_press(int mx, int my,
                                       uint32_t btn_mask, int btn_idx) {
    struct window *popup = find_by_kind(WM_KIND_POPUP);
    if (popup) {
        if (mx >= popup->x && mx < popup->x + popup->w &&
            my >= popup->y && my < popup->y + popup->h) {
            int cx = mx - cli_x(popup);
            int cy = my - cli_y(popup);
            g_mouse_down_win[btn_idx] = popup;
            send_event(popup, WM_EV_MOUSE_DOWN, btn_mask, cx, cy, g_mods);
            return;
        }
        destroy_window(popup);
    }

    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (dock && mx >= dock->x && mx < dock->x + dock->w &&
        my >= dock->y && my < dock->y + dock->h) {
        int cx = mx - cli_x(dock);
        int cy = my - cli_y(dock);
        g_mouse_down_win[btn_idx] = dock;
        send_event(dock, WM_EV_MOUSE_DOWN, btn_mask, cx, cy, g_mods);
        return;
    }

    struct window *hit = find_top_normal_window(mx, my);
    if (hit) {
        int need_full = (hit != g_focus);
        set_focus(hit);
        hit->z = g_next_z++;

        int cx = mx - cli_x(hit);
        int cy = my - cli_y(hit);
        if (cx >= 0 && cx < hit->cw && cy >= 0 && cy < hit->ch) {
            g_mouse_down_win[btn_idx] = hit;
            send_event(hit, WM_EV_MOUSE_DOWN, btn_mask, cx, cy, g_mods);
        }
        if (need_full) {
            mouse_restore();
            redraw_region(0, 0, g_W, g_H);
            mouse_draw();
        }
        return;
    }

    struct window *wall = find_by_kind(WM_KIND_WALLPAPER);
    if (wall) {
        set_focus(NULL);
        int cx = mx - cli_x(wall);
        int cy = my - cli_y(wall);
        g_mouse_down_win[btn_idx] = wall;
        send_event(wall, WM_EV_MOUSE_DOWN, btn_mask, cx, cy, g_mods);
    }
}

static void handle_other_button_release(int mx, int my,
                                         uint32_t btn_mask, int btn_idx) {
    struct window *w = g_mouse_down_win[btn_idx];
    if (!w) return;
    g_mouse_down_win[btn_idx] = NULL;
    if (!w->used) return;

    int cx = mx - cli_x(w);
    int cy = my - cli_y(w);
    send_event(w, WM_EV_MOUSE_UP, btn_mask, cx, cy, g_mods);
}

/* ==================== inbox ==================== */

static void handle_inbox(void) {
    while (g_inbox_rd >= 0 && pipe_avail(g_inbox_rd) >= 8) {
        uint32_t hdr[2];
        int n = pipe_read(g_inbox_rd, hdr, 8);
        if (n != 8) break;
        if (hdr[0] != WM_MAGIC) continue;

        if (hdr[1] == WM_REQ_CREATE) {
            struct wm_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            make_app_window(&req);
        } else if (hdr[1] == WM_REQ_DESTROY) {
            struct wm_destroy_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            struct window *w = find_by_shm(req.shm_name);
            if (w) destroy_window(w);
        } else if (hdr[1] == WM_REQ_WIN_ACTION) {
            struct wm_action_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;

            if (req.win_id >= MAX_WINDOWS) continue;
            struct window *w = &g_wins[req.win_id];
            if (!w->used || w->kind != WM_KIND_NORMAL) continue;

            if (req.action == WM_ACTION_MINIMIZE) {
                minimize_window(w);
            } else if (req.action == WM_ACTION_RESTORE) {
                if (w->minimized) restore_window(w);
                else if (w->maximized) unmaximize_window(w);
            } else if (req.action == WM_ACTION_RAISE) {
                w->z = g_next_z++;
                set_focus(w);
                mouse_restore();
                redraw_region(w->x, w->y, w->x + w->w + 4, w->y + w->h + 4);
                mouse_draw();
            } else if (req.action == WM_ACTION_MAXIMIZE) {
                if (w->maximized) unmaximize_window(w);
                else              maximize_window(w);
            }
        } else if (hdr[1] == WM_REQ_RESIZE) {
            struct wm_resize_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            handle_resize_request(&req);
        } else if (hdr[1] == WM_REQ_SET_ALPHA) {
            struct wm_set_alpha_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            struct window *w = find_by_shm(req.shm_name);
            if (w && w->owner_pid == (int)req.owner_pid) {
                w->alpha = req.alpha > 255 ? 255 : req.alpha;
                int x0, y0, x1, y1;
                win_bbox(w, &x0, &y0, &x1, &y1);
                mouse_restore();
                redraw_region(x0, y0, x1, y1);
                mouse_draw();
            }
        } else if (hdr[1] == WM_REQ_SET_CURSOR) {
            struct wm_set_cursor_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (g_wins[i].used &&
                    g_wins[i].owner_pid == (int)req.owner_pid) {
                    g_wins[i].cursor = req.cursor;
                    break;
                }
            }
        } else {
            continue;
        }
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
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((int)back_addr <= 0) { printf("wm: back mmap failed\n"); return 1; }
    g_back = (unsigned int*)back_addr;

    unsigned int scratch_addr = mmap(0, back_size,
                                     PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((int)scratch_addr > 0) g_scratch = (unsigned int*)scratch_addr;

    memset(g_back, 0, back_size);
    fill_rect_buf(g_back, 0, 0, g_W, g_H, COL_BG);
    reset_clip();

    pipe_unlink(WM_INBOX_NAME);
    int inbox_fds[2];
    if (pipe_named(WM_INBOX_NAME, inbox_fds) < 0) {
        printf("wm: wm-inbox create failed\n");
        return 1;
    }
    g_inbox_rd = inbox_fds[0];
    ipc_close(inbox_fds[1]);

    wm_font_load();

    blit_back_to_fb(0, 0, g_W, g_H);
    mouse_x = g_W / 2;
    mouse_y = g_H / 2;
    mouse_draw();
    printf("wm: ready\n");

    struct window *drag_win = NULL;
    int drag_dx = 0, drag_dy = 0;

    for (;;) {
        struct input_event ev;
        int have_last_mouse = 0;
        int last_mx = 0, last_my = 0;

        while (input_poll(&ev) == 0) {
            g_mods = ev.mods;

            if (ev.type == INPUT_KEY) {
                struct window *kt = g_focus;
                if (!kt) kt = find_by_kind(WM_KIND_WALLPAPER);
                if (kt && kt->event_w >= 0) {
                    send_event(kt, WM_EV_KEY, ev.code, 0, 0, g_mods);
                }
            } else if (ev.type == INPUT_WHEEL) {
                struct window *wt = window_at(ev.x, ev.y);
                if (wt && wt->event_w >= 0) {
                    int cx = ev.x - cli_x(wt);
                    int cy = ev.y - cli_y(wt);
                    send_event(wt, WM_EV_WHEEL, ev.code, cx, cy, g_mods);
                }
            } else if (ev.type == INPUT_MOUSE_MOVE) {
                last_mx = ev.x;
                last_my = ev.y;
                have_last_mouse = 1;
            } else if (ev.type == INPUT_MOUSE_BTN) {
                uint32_t old = g_btn_state;
                uint32_t now = ev.code & 0x07;
                uint32_t pressed  = now & ~old;
                uint32_t released = old & ~now;
                g_btn_state = now;

                if (pressed & WM_BTN_LEFT) {
                    handle_mouse_press(ev.x, ev.y,
                                       &drag_win, &drag_dx, &drag_dy);
                }
                if (released & WM_BTN_LEFT) {
                    if (drag_win) {
                        drag_win = NULL;
                        g_dragging = NULL;
                    } else if (g_mouse_down_win[0]) {
                        struct window *src = g_mouse_down_win[0];
                        struct window *dst = window_at(ev.x, ev.y);
                        uint32_t up_extra = g_mods;
                        if (dst && dst != src) up_extra |= WM_MOD_DROP_OUTSIDE;

                        if (src->used) {
                            int cx = ev.x - cli_x(src);
                            int cy = ev.y - cli_y(src);
                            send_event(src, WM_EV_MOUSE_UP,
                                       WM_BTN_LEFT, cx, cy, up_extra);
                        }
                        g_mouse_down_win[0] = NULL;

                        if (dst && dst != src && dst->used &&
                            (dst->kind == WM_KIND_NORMAL ||
                             dst->kind == WM_KIND_WALLPAPER)) {
                            int dx = ev.x - cli_x(dst);
                            int dy = ev.y - cli_y(dst);
                            send_event(dst, WM_EV_DROP, 0, dx, dy, g_mods);
                        }
                    }
                }

                if (pressed & WM_BTN_RIGHT) {
                    handle_other_button_press(ev.x, ev.y,
                                              WM_BTN_RIGHT, 1);
                }
                if (released & WM_BTN_RIGHT) {
                    handle_other_button_release(ev.x, ev.y,
                                                WM_BTN_RIGHT, 1);
                }

                if (pressed & WM_BTN_MIDDLE) {
                    handle_other_button_press(ev.x, ev.y,
                                              WM_BTN_MIDDLE, 2);
                }
                if (released & WM_BTN_MIDDLE) {
                    handle_other_button_release(ev.x, ev.y,
                                                WM_BTN_MIDDLE, 2);
                }
            }
        }

        if (have_last_mouse) {
            if (drag_win) {
                int nx = last_mx - drag_dx;
                int ny = last_my - drag_dy;
                if (nx < 0) nx = 0;
                if (ny < 0) ny = 0;
                if (nx + drag_win->w + 4 > (int)g_W)
                    nx = g_W - drag_win->w - 4;
                if (ny + drag_win->h + 4 > (int)g_H - dock_height())
                    ny = g_H - dock_height() - drag_win->h - 4;

                if (nx != drag_win->x || ny != drag_win->y) {
                    mouse_restore();

                    int ox0, oy0, ox1, oy1;
                    win_bbox(drag_win, &ox0, &oy0, &ox1, &oy1);
                    drag_win->x = nx;
                    drag_win->y = ny;
                    int nx0, ny0, nx1, ny1;
                    win_bbox(drag_win, &nx0, &ny0, &nx1, &ny1);

                    int ix0 = ox0 > nx0 ? ox0 : nx0;
                    int iy0 = oy0 > ny0 ? oy0 : ny0;
                    int ix1 = ox1 < nx1 ? ox1 : nx1;
                    int iy1 = oy1 < ny1 ? oy1 : ny1;
                    int has_intersect = (ix0 < ix1 && iy0 < iy1);

                    g_dragging = drag_win;
                    g_no_present = 1;

                    if (has_intersect) {
                        if (oy0 < iy0) redraw_region(ox0, oy0, ox1, iy0);
                        if (iy1 < oy1) redraw_region(ox0, iy1, ox1, oy1);
                        if (ox0 < ix0) redraw_region(ox0, iy0, ix0, iy1);
                        if (ix1 < ox1) redraw_region(ix1, iy0, ox1, iy1);
                    } else {
                        redraw_region(ox0, oy0, ox1, oy1);
                    }

                    if (has_intersect) {
                        if (ny0 < iy0) redraw_region(nx0, ny0, nx1, iy0);
                        if (iy1 < ny1) redraw_region(nx0, iy1, nx1, ny1);
                        if (nx0 < ix0) redraw_region(nx0, iy0, ix0, iy1);
                        if (ix1 < nx1) redraw_region(ix1, iy0, nx1, iy1);
                    } else {
                        redraw_region(nx0, ny0, nx1, ny1);
                    }

                    set_clip(0, 0, (int)g_W, (int)g_H);
                    render_win_frame(drag_win);
                    blit_shm_to_back(drag_win, nx0, ny0, nx1, ny1);

                    g_dragging = NULL;
                    g_no_present = 0;

                    int ux0 = ox0 < nx0 ? ox0 : nx0;
                    int uy0 = oy0 < ny0 ? oy0 : ny0;
                    int ux1 = ox1 > nx1 ? ox1 : nx1;
                    int uy1 = oy1 > ny1 ? oy1 : ny1;
                    blit_back_to_fb(ux0, uy0, ux1, uy1);

                    mouse_x = last_mx;
                    mouse_y = last_my;
                    mouse_draw();
                } else if (last_mx != mouse_x || last_my != mouse_y) {
                    mouse_restore();
                    mouse_x = last_mx;
                    mouse_y = last_my;
                    mouse_draw();
                }
            } else {
                if (last_mx != mouse_x || last_my != mouse_y) {
                    mouse_restore();
                    mouse_x = last_mx;
                    mouse_y = last_my;
                    mouse_draw();
                }
                route_mouse_move(last_mx, last_my);
            }
        }

        handle_inbox();

        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used || !w->closing) continue;
            if (g_wm_tick - w->close_tick > 500) {
                destroy_window(w);
            }
        }

        int mouse_hidden = (mouse_prev_x < 0);
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used) continue;
            if (w->minimized) { w->last_seen = *w->counter; shm_clear_pub(w); continue; }

            uint64_t c = *w->counter;
            if (drag_win) { w->last_seen = c; shm_clear_pub(w); continue; }

            int pub_empty = 1;
            if (w->shm)
                pub_empty = (w->shm->pub_x0 == SHM_DIRTY_EMPTY);
            if (c == w->last_seen && pub_empty) continue;
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
            if (dx0 >= (uint32_t)cw || dy0 >= (uint32_t)ch) {
                w->shm->pub_x0 = SHM_DIRTY_EMPTY;
                continue;
            }
            if (dx1 > (uint32_t)cw) dx1 = cw;
            if (dy1 > (uint32_t)ch) dy1 = ch;
            if (dx1 <= dx0 || dy1 <= dy0) {
                w->shm->pub_x0 = SHM_DIRTY_EMPTY;
                continue;
            }

            int sx0 = cli_x(w) + dx0;
            int sy0 = cli_y(w) + dy0;
            int sx1 = cli_x(w) + dx1;
            int sy1 = cli_y(w) + dy1;

            int occluded = (w->alpha > 0 && w->alpha < 255) ? 1 : 0;
            for (int j = 0; j < MAX_WINDOWS; j++) {
                struct window *o = &g_wins[j];
                if (!o->used || o == w || o->minimized) continue;
                if (layer_of(o) < layer_of(w)) continue;
                if (layer_of(o) == layer_of(w) && o->z <= w->z) continue;
                if (o->x >= sx1 || o->x + o->w <= sx0) continue;
                if (o->y >= sy1 || o->y + o->h <= sy0) continue;
                occluded = 1;
                break;
            }

            if (!mouse_hidden && mouse_prev_x >= 0 &&
                mouse_intersects_rect(sx0, sy0, sx1, sy1)) {
                mouse_restore();
                mouse_hidden = 1;
            }

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

            w->shm->pub_x0 = SHM_DIRTY_EMPTY;
            w->shm->pub_y0 = 0;
            w->shm->pub_x1 = 0;
            w->shm->pub_y1 = 0;
        }

        if (mouse_hidden) mouse_draw();

        static int check_cnt = 0;
        if (++check_cnt >= 100) {
            check_cnt = 0;
            cleanup_dead_windows();
        }

        g_wm_tick++;
        sleep_ms(2);
    }

    return 0;
}