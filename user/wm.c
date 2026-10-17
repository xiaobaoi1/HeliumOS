/* wm — 窗口管理器（Desktop 版） */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include <stdint.h>
#include "signal.h"
#include "wm_proto.h"
#include "wm_theme.h"
#include "wm_font.h"
#include "shm_layout.h"

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

#define MAX_WINDOWS 8

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
    uint64_t *counter;
    uint64_t last_seen;
    struct shm_hdr *shm;

    unsigned int map_addr;   /* 新增 */
    unsigned int map_size;   /* 新增 */

    int event_w;
    int minimized;
};

static int g_inbox_rd = -1;
static struct window g_wins[MAX_WINDOWS];
static int g_next_z = 1;
static struct window *g_focus = NULL;

static int cli_x(struct window *w) {
    return w->kind == WM_KIND_NORMAL ? w->x + BORDER : w->x;
}
static int cli_y(struct window *w) {
    return w->kind == WM_KIND_NORMAL ? w->y + TITLE_H : w->y;
}
static int cli_w(struct window *w) { return w->cw; }
static int cli_h(struct window *w) { return w->ch; }

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

static void draw_title_text(struct window *w);

static void render_win_frame(struct window *w) {
    if (w->kind != WM_KIND_NORMAL) return;

    fill_rect_buf(g_back, w->x + 4, w->y + 4, w->w, w->h, 0x000000);
    fill_rect_buf(g_back, w->x, w->y, w->w, TITLE_H, COL_TITLE);
    fill_rect_buf(g_back, w->x, w->y, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y + w->h - BORDER, w->w, BORDER, COL_BORDER);
    fill_rect_buf(g_back, w->x, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, w->x + w->w - BORDER, w->y, BORDER, w->h, COL_BORDER);
    fill_rect_buf(g_back, cli_x(w), cli_y(w), cli_w(w), cli_h(w), 0x000000);

    draw_title_text(w);

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
    if (!wm_font_loaded()) return;

    int tx = w->x + 8;
    int ty = w->y + (TITLE_H - FONT_H) / 2;

    for (const char *p = w->title; *p; p++) {
        int cp = (unsigned char)*p;
        if (cp >= 128) cp = '?';

        const uint8_t *g = wm_font_glyph(cp);
        if (g) {
            for (int r = 0; r < FONT_H; r++) {
                unsigned char bits = g[r];
                for (int j = 0; j < FONT_W; j++) {
                    if (bits & (0x80 >> j)) {
                        put_pixel_buf(g_back, tx + j, ty + r, COL_TITLE_FG);
                    }
                }
            }
        }
        tx += FONT_W;
        if (tx + FONT_W > w->x + w->w - 28) break;
    }
}

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

    /* 1. 壁纸 */
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

    /* 2-4. 按层合成 */
    for (int layer = 1; layer <= 3; layer++) {
        for (int z = 0; z <= g_next_z; z++) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                struct window *w = &g_wins[i];
                if (!w->used || w->minimized) continue;
                if (layer_of(w) != layer) continue;
                if (w->z != z) continue;
                if (layer == 1) {
                    if (w->x + 4 >= x1 || w->x + w->w + 4 <= x0) continue;
                    if (w->y + 4 >= y1 || w->y + w->h + 4 <= y0) continue;
                    render_win_frame(w);
                } else {
                    if (w->x >= x1 || w->x + w->w <= x0) continue;
                    if (w->y >= y1 || w->y + w->h <= y0) continue;
                }
                blit_shm_to_back(w);
            }
        }
    }

    blit_back_to_fb(x0, y0, x1, y1);
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

/* ==================== 事件 ==================== */

static void send_event(struct window *w, uint32_t type, uint32_t code,
                       int x, int y) {
    if (!w || w->event_w < 0) return;
    struct wm_event ev = { type, code, x, y };
    pipe_write(w->event_w, &ev, sizeof(ev));
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

static void push_window_list(void) {
    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (!dock || dock->event_w < 0) return;

    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].used && g_wins[i].kind == WM_KIND_NORMAL) n++;
    }
    if (n > 8) n = 8;

    uint8_t buf[16 + 8 * 40];
    struct wm_event *ev = (struct wm_event*)buf;
    ev->type = WM_EV_WINDOW_LIST;
    ev->code = (uint32_t)n;
    ev->x = 0;
    ev->y = 0;

    int off = 16;
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS && count < n; i++) {
        struct window *w = &g_wins[i];
        if (!w->used || w->kind != WM_KIND_NORMAL) continue;
        struct wm_window_entry *e = (struct wm_window_entry*)(buf + off);
        e->id = (uint32_t)i;
        e->minimized = (uint32_t)w->minimized;
        memset(e->title, 0, 32);
        strncpy(e->title, w->title, 31);
        off += 40;
        count++;
    }

    pipe_write(dock->event_w, buf, off);
}

/* ==================== 窗口创建/销毁 ==================== */

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
    strncpy(w->title, req->title, sizeof(w->title) - 1);
    strncpy(w->shm_name, req->shm_name, sizeof(w->shm_name) - 1);

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
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { w->used = 0; return NULL; }

    w->shm_idx = shm_id;
    w->shm = (struct shm_hdr*)addr;
    w->map_addr = addr;
    w->map_size = shm_size;
    w->counter = &w->shm->counter;
    w->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    w->last_seen = *w->counter;

    int ev_w = pipe_open(req->ev_name, 1);
    if (ev_w < 0) { w->used = 0; return NULL; }
    w->event_w = ev_w;

    mouse_restore();
    int bx0, by0, bx1, by1;
    win_bbox(w, &bx0, &by0, &bx1, &by1);
    redraw_region(bx0, by0, bx1, by1);
    mouse_draw();
    push_window_list();
    return w;
}

static void minimize_window(struct window *w) {
    if (!w || w->minimized) return;
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);
    w->minimized = 1;
    if (g_focus == w) g_focus = NULL;
    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void restore_window(struct window *w) {
    if (!w || !w->minimized) return;
    w->minimized = 0;
    w->z = g_next_z++;
    g_focus = w;
    mouse_restore();
    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void destroy_window(struct window *w) {
    if (!w || !w->used) return;

    int x0, y0, x1, y1;
    win_bbox(w, &x0, &y0, &x1, &y1);

    if (w->kind == WM_KIND_POPUP) {
        send_event(w, WM_EV_CLOSE, 0, 0, 0);
    }

    if (w->event_w >= 0) ipc_close(w->event_w);

    if (w->map_addr) {
        munmap(w->map_addr, w->map_size);
        w->map_addr = 0;
        w->map_size = 0;
    }
    if (w->shm_name[0]) shm_unlink(w->shm_name);

    w->used = 0;
    w->event_w = -1;
    if (g_focus == w) g_focus = NULL;

    mouse_restore();
    redraw_region(x0, y0, x1, y1);
    mouse_draw();
    push_window_list();
}

static void handle_resize_request(struct wm_resize_request *req) {
    struct window *w = find_by_shm(req->old_shm);
    if (!w) return;
    if (req->new_w == 0 || req->new_h == 0) return;
    if (req->new_w > 2048 || req->new_h > 1536) return;

    int ox0, oy0, ox1, oy1;
    win_bbox(w, &ox0, &oy0, &ox1, &oy1);

    int cw = req->new_w;
    int ch = req->new_h;
    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(req->new_shm, shm_size, 0);
    if (shm_id < 0) return;

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
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

    if (target) {
        int cx = mx - cli_x(target);
        int cy = my - cli_y(target);
        send_event(target, WM_EV_MOUSE_MOVE, 0, cx, cy);
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
            send_event(popup, WM_EV_MOUSE_BTN, 1, cx, cy);
            return;
        }
        destroy_window(popup);
    }

    struct window *dock = find_by_kind(WM_KIND_DOCK);
    if (dock && mx >= dock->x && mx < dock->x + dock->w &&
        my >= dock->y && my < dock->y + dock->h) {
        int cx = mx - cli_x(dock);
        int cy = my - cli_y(dock);
        send_event(dock, WM_EV_MOUSE_BTN, 1, cx, cy);
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
        g_focus = hit;
        hit->z = g_next_z++;

        int bx = hit->x + hit->w - CLOSE_BTN_W - 4;
        int by = hit->y + 4;
        if (mx >= bx && mx < bx + CLOSE_BTN_W &&
            my >= by && my < by + CLOSE_BTN_W) {
            send_event(hit, WM_EV_CLOSE, 0, 0, 0);
            destroy_window(hit);
        } else if (my >= hit->y && my < hit->y + TITLE_H) {
            *drag_win = hit;
            *drag_dx = mx - hit->x;
            *drag_dy = my - hit->y;
            if (need_full) {
                mouse_restore();
                redraw_region(0, 0, g_W, g_H);
                mouse_draw();
            }
        } else {
            int cx = mx - cli_x(hit);
            int cy = my - cli_y(hit);
            send_event(hit, WM_EV_MOUSE_BTN, 1, cx, cy);
            if (need_full) {
                mouse_restore();
                redraw_region(0, 0, g_W, g_H);
                mouse_draw();
            }
        }
        return;
    }

    struct window *wall = find_by_kind(WM_KIND_WALLPAPER);
    if (wall) {
        int cx = mx - cli_x(wall);
        int cy = my - cli_y(wall);
        send_event(wall, WM_EV_MOUSE_BTN, 1, cx, cy);
    }
}

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

            if (req.action == WM_ACTION_MINIMIZE) minimize_window(w);
            else if (req.action == WM_ACTION_RESTORE) restore_window(w);
            else if (req.action == WM_ACTION_RAISE) {
                w->z = g_next_z++;
                g_focus = w;
                mouse_restore();
                redraw_region(w->x, w->y, w->x + w->w + 4, w->y + w->h + 4);
                mouse_draw();
            }
        } else if (hdr[1] == WM_REQ_RESIZE) {
            struct wm_resize_request req;
            req.magic = hdr[0];
            req.type = hdr[1];
            int rem = sizeof(req) - 8;
            if (pipe_avail(g_inbox_rd) < rem) break;
            if (pipe_read(g_inbox_rd, (char*)&req + 8, rem) != rem) break;
            handle_resize_request(&req);
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
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1);
    if ((int)back_addr <= 0) { printf("wm: back mmap failed\n"); return 1; }
    g_back = (unsigned int*)back_addr;
    memset(g_back, 0, back_size);
    fill_rect_buf(g_back, 0, 0, g_W, g_H, COL_BG);

    wm_font_load();

    pipe_unlink(WM_INBOX_NAME);
    int inbox_fds[2];
    if (pipe_named(WM_INBOX_NAME, inbox_fds) < 0) {
        printf("wm: wm-inbox create failed\n");
        return 1;
    }
    g_inbox_rd = inbox_fds[0];
    ipc_close(inbox_fds[1]);

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
            if (ev.type == INPUT_KEY) {
                if (g_focus && g_focus->event_w >= 0) {
                    send_event(g_focus, WM_EV_KEY, ev.code, 0, 0);
                }
            } else if (ev.type == INPUT_MOUSE_MOVE) {
                last_mx = ev.x;
                last_my = ev.y;
                have_last_mouse = 1;
            } else if (ev.type == INPUT_MOUSE_BTN) {
                int pressed = (ev.code & 0x01) != 0;
                if (pressed) {
                    handle_mouse_press(ev.x, ev.y,
                                       &drag_win, &drag_dx, &drag_dy);
                } else {
                    if (drag_win) {
                        drag_win = NULL;
                    } else {
                        struct window *popup = find_by_kind(WM_KIND_POPUP);
                        if (popup) {
                            int cx = ev.x - cli_x(popup);
                            int cy = ev.y - cli_y(popup);
                            send_event(popup, WM_EV_MOUSE_BTN, 0, cx, cy);
                        }
                        struct window *dock = find_by_kind(WM_KIND_DOCK);
                        if (dock && ev.x >= dock->x &&
                            ev.x < dock->x + dock->w &&
                            ev.y >= dock->y &&
                            ev.y < dock->y + dock->h) {
                            int cx = ev.x - cli_x(dock);
                            int cy = ev.y - cli_y(dock);
                            send_event(dock, WM_EV_MOUSE_BTN, 0, cx, cy);
                        }
                    }
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
                if (ny + drag_win->h + 4 > (int)g_H)
                    ny = g_H - drag_win->h - 4;

                if (nx != drag_win->x || ny != drag_win->y) {
                    mouse_restore();
                    int ox0, oy0, ox1, oy1;
                    win_bbox(drag_win, &ox0, &oy0, &ox1, &oy1);
                    drag_win->x = nx;
                    drag_win->y = ny;
                    int nx0, ny0, nx1, ny1;
                    win_bbox(drag_win, &nx0, &ny0, &nx1, &ny1);
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
                route_mouse_move(last_mx, last_my);
            }
        }

        handle_inbox();

        /* 窗口 shm 更新 */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_wins[i];
            if (!w->used) continue;
            if (w->minimized) { w->last_seen = *w->counter; continue; }

            uint64_t c = *w->counter;
            if (drag_win) { w->last_seen = c; continue; }
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
                if (!o->used || o == w || o->minimized) continue;
                if (layer_of(o) < layer_of(w)) continue;
                if (layer_of(o) == layer_of(w) && o->z <= w->z) continue;
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

        static int check_cnt = 0;
        if (++check_cnt >= 100) {
            check_cnt = 0;
            cleanup_dead_windows();
        }

        sleep_ms(2);
    }

    return 0;
}