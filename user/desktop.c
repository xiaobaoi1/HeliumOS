/* desktop — 壁纸 + 任务栏 + 开始菜单 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include "wm_proto.h"
#include "wm_font.h"
#include "shm_layout.h"

#define PANEL_H     28
#define MENU_ITEM_H 24
#define MENU_PAD    4
#define MENU_W      160

#define COL_PANEL      0x2A2A3E
#define COL_PANEL_FG   0xCCCCCC
#define COL_MENU_BG    0x2A2A3E
#define COL_MENU_FG    0xFFFFFF
#define COL_MENU_HL    0x3A6EA5

// #define WALL_TOP       0x1A1A2E
// #define WALL_BOTTOM    0x16213E

#define WALL_TOP       0x303A3E
#define WALL_BOTTOM    0x102140

static int g_W, g_H;
static int g_inbox_w = -1;

static unsigned int *g_wall_pixels;
static struct shm_hdr *g_wall_hdr;
static char g_wall_shm[16];
static int g_wall_ev = -1;

static unsigned int *g_panel_pixels;
static struct shm_hdr *g_panel_hdr;
static char g_panel_shm[16];
static int g_panel_ev = -1;

static unsigned int *g_menu_pixels;
static struct shm_hdr *g_menu_hdr;
static char g_menu_shm[16];
static int g_menu_ev = -1;
static int g_menu_w, g_menu_h, g_menu_visible;

struct desktop_win {
    uint32_t id;
    uint32_t minimized;
    char     title[32];
};

static struct desktop_win g_win_list[8];
static int g_win_count = 0;

#define WIN_ITEM_X0   64
#define WIN_ITEM_W    100

static unsigned int lerp(unsigned int a, unsigned int b, int t, int max) {
    if (max <= 0) return a;
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ar + (br - ar) * t / max;
    int g = ag + (bg - ag) * t / max;
    int bl = ab + (bb - ab) * t / max;
    return (r << 16) | (g << 8) | bl;
}

static void draw_text(unsigned int *pixels, int pw, int px, int py,
                      const char *s, unsigned int fg, unsigned int bg) {
    for (const char *p = s; *p; p++) {
        int cp = (unsigned char)*p;
        if (cp >= 128) cp = '?';
        const uint8_t *g = wm_font_glyph(cp);
        for (int r = 0; r < 16; r++) {
            unsigned char bits = g ? g[r] : 0;
            for (int j = 0; j < 8; j++) {
                int x = px + j;
                int y = py + r;
                if (x < 0 || x >= pw) continue;
                unsigned int c = (bits & (0x80 >> j)) ? fg : bg;
                pixels[y * pw + x] = c;
            }
        }
        px += 8;
    }
}

/* 创建 WM 窗口 + shm + ev pipe。返回 0 成功，像素和 hdr 通过 out 参数。 */
static int create_wm_window(const char *tag, int kind, int x, int y,
                            int w, int h, int is_popup,
                            unsigned int **out_pixels,
                            struct shm_hdr **out_hdr,
                            char *out_shm_name, char *out_ev_name,
                            int *out_ev_rd) {
    int shm_size = shm_total_size(w, h);
    int shm_id = shm_open(out_shm_name, shm_size, 0);
    if (shm_id < 0) return -1;

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) return -1;

    struct shm_hdr *hdr = (struct shm_hdr*)addr;
    unsigned int *pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(pixels, 0, w * h * 4);
    hdr->counter = 0;
    hdr->cur_x0 = SHM_DIRTY_EMPTY; hdr->cur_y0 = 0;
    hdr->cur_x1 = 0; hdr->cur_y1 = 0;
    hdr->pub_x0 = SHM_DIRTY_EMPTY; hdr->pub_y0 = 0;
    hdr->pub_x1 = 0; hdr->pub_y1 = 0;

    pipe_unlink(out_ev_name);
    int fds[2];
    if (pipe_named(out_ev_name, fds) < 0) return -1;
    *out_ev_rd = fds[0];
    ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type = WM_REQ_CREATE;
    req.kind = kind;
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    req.owner_pid = getpid();
    req.min_w = req.min_h = req.max_w = req.max_h = 0;
    strncpy(req.shm_name, out_shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, out_ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, tag, sizeof(req.title) - 1);

    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return -1;

    *out_pixels = pixels;
    *out_hdr = hdr;
    return 0;
}

static void draw_wallpaper(void) {
    for (int y = 0; y < g_H; y++) {
        unsigned int c = lerp(WALL_TOP, WALL_BOTTOM, y, g_H);
        for (int x = 0; x < g_W; x++) {
            g_wall_pixels[y * g_W + x] = c;
        }
    }
    g_wall_hdr->cur_x0 = 0;
    g_wall_hdr->cur_y0 = 0;
    g_wall_hdr->cur_x1 = g_W;
    g_wall_hdr->cur_y1 = g_H;
    g_wall_hdr->counter++;
}

static void draw_window_items(void) {
    int x = WIN_ITEM_X0;
    for (int i = 0; i < g_win_count; i++) {
        if (x + WIN_ITEM_W > g_W - 80) break;
        unsigned int bg = g_win_list[i].minimized ? 0x1A1A2E : 0x3A4A5E;
        for (int y = 0; y < PANEL_H; y++)
            for (int j = 0; j < WIN_ITEM_W - 4; j++)
                g_panel_pixels[y * g_W + x + j] = bg;

        char label[13];
        int k = 0;
        for (; k < 12 && g_win_list[i].title[k]; k++)
            label[k] = g_win_list[i].title[k];
        label[k] = '\0';

        draw_text(g_panel_pixels, g_W, x + 4, (PANEL_H - 16) / 2,
                  label, COL_PANEL_FG, bg);
        x += WIN_ITEM_W;
    }
}

static void draw_clock_region(void) {
    int x0 = g_W - 80;
    if (x0 < 0) x0 = 0;
    for (int y = 0; y < PANEL_H; y++)
        for (int x = x0; x < g_W; x++)
            g_panel_pixels[y * g_W + x] = COL_PANEL;

    struct rtc_time t;
    if (rtc_get_time(&t) == 0) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 t.hour, t.minute, t.second);
        draw_text(g_panel_pixels, g_W, x0 + 4, (PANEL_H - 16) / 2,
                  buf, COL_PANEL_FG, COL_PANEL);
    }
}

static void draw_panel(void) {
    for (int i = 0; i < g_W * PANEL_H; i++)
        g_panel_pixels[i] = COL_PANEL;

    draw_text(g_panel_pixels, g_W, 8, (PANEL_H - 16) / 2,
              "Start", COL_PANEL_FG, COL_PANEL);

    draw_window_items();
    draw_clock_region();

    g_panel_hdr->cur_x0 = 0;
    g_panel_hdr->cur_y0 = 0;
    g_panel_hdr->cur_x1 = g_W;
    g_panel_hdr->cur_y1 = PANEL_H;
    g_panel_hdr->counter++;
}

static void update_clock(void) {
    draw_clock_region();
    int x0 = g_W - 80;
    if (x0 < 0) x0 = 0;
    g_panel_hdr->cur_x0 = x0;
    g_panel_hdr->cur_y0 = 0;
    g_panel_hdr->cur_x1 = g_W;
    g_panel_hdr->cur_y1 = PANEL_H;
    g_panel_hdr->counter++;
}

/* ---------- 菜单 ---------- */

#define MENU_ITEMS 3
static const char *g_menu_labels[MENU_ITEMS] = {
    "Terminal", "Run TEST", "About"
};

static void draw_menu(int hover) {
    for (int i = 0; i < g_menu_w * g_menu_h; i++)
        g_menu_pixels[i] = COL_MENU_BG;

    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = MENU_PAD + i * MENU_ITEM_H;
        unsigned int bg = (i == hover) ? COL_MENU_HL : COL_MENU_BG;
        if (i == hover) {
            for (int j = 0; j < MENU_ITEM_H; j++)
                for (int k = 0; k < g_menu_w; k++)
                    g_menu_pixels[(y + j) * g_menu_w + k] = COL_MENU_HL;
        }
        draw_text(g_menu_pixels, g_menu_w, 8, y + (MENU_ITEM_H - 16) / 2,
                  g_menu_labels[i], COL_MENU_FG, bg);
    }

    g_menu_hdr->cur_x0 = 0;
    g_menu_hdr->cur_y0 = 0;
    g_menu_hdr->cur_x1 = g_menu_w;
    g_menu_hdr->cur_y1 = g_menu_h;
    g_menu_hdr->counter++;
}

static void spawn_term(int cw, int ch, int x, int y) {
    static int term_slot = 0;
    char shm_name[16], ev_name[24];
    snprintf(shm_name, sizeof(shm_name), "term-%d", term_slot);
    snprintf(ev_name, sizeof(ev_name), "wm-term-%d", term_slot);

    int shm_size = shm_total_size(cw, ch);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) return;

    pipe_unlink(ev_name);
    int fds[2];
    if (pipe_named(ev_name, fds) < 0) return;
    ipc_close(fds[0]);
    ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type = WM_REQ_CREATE;
    req.kind = WM_KIND_NORMAL;
    req.x = x;
    req.y = y;
    req.w = cw;
    req.h = ch;
    req.owner_pid = getpid();
    strncpy(req.shm_name, shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, ev_name, sizeof(req.ev_name) - 1);
    strcpy(req.title, "Terminal");
    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return;

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
        .child_class = CLASS_USER,
        .child_caps = 0,
    };
    spawn("TERM", argv, &sp);

    term_slot++;
}

static void open_start_menu(void) {
    if (g_menu_visible) return;

    g_menu_w = MENU_W;
    g_menu_h = MENU_ITEMS * MENU_ITEM_H + MENU_PAD * 2;
    int x = 4;
    int y = g_H - PANEL_H - g_menu_h;

    strcpy(g_menu_shm, "menu-shm");
    char ev_name[24];
    strcpy(ev_name, "menu-ev");

    if (create_wm_window("", WM_KIND_POPUP, x, y, g_menu_w, g_menu_h, 1,
                         &g_menu_pixels, &g_menu_hdr,
                         g_menu_shm, ev_name, &g_menu_ev) < 0)
        return;

    draw_menu(-1);
    g_menu_visible = 1;
}

static void close_menu_request(void) {
    if (!g_menu_visible) return;
    struct wm_destroy_request dr;
    dr.magic = WM_MAGIC;
    dr.type = WM_REQ_DESTROY;
    dr.owner_pid = getpid();
    strcpy(dr.shm_name, g_menu_shm);
    pipe_write(g_inbox_w, &dr, sizeof(dr));
}

static void handle_menu_event(struct wm_event *ev) {
    if (ev->type == WM_EV_CLOSE) {
        if (g_menu_hdr) {
            munmap((unsigned int)g_menu_hdr,
                   shm_total_size(g_menu_w, g_menu_h));
            g_menu_hdr = NULL;
            g_menu_pixels = NULL;
        }
        shm_unlink(g_menu_shm);
        pipe_unlink("menu-ev");
        if (g_menu_ev >= 0) {
            ipc_close(g_menu_ev);
            g_menu_ev = -1;
        }
        g_menu_visible = 0;
        return;
    }
    if (ev->type == WM_EV_MOUSE_MOVE) {
        int item = (ev->y - MENU_PAD) / MENU_ITEM_H;
        if (item >= 0 && item < MENU_ITEMS) draw_menu(item);
        else draw_menu(-1);
        return;
    }
    if (ev->type == WM_EV_MOUSE_DOWN && ev->code == 1) {
        int item = (ev->y - MENU_PAD) / MENU_ITEM_H;
        if (item == 0) spawn_term(600, 400, 200, 100);
        else if (item == 1) spawn("TEST", NULL, NULL);
        close_menu_request();
    }
}

static void handle_panel_event(struct wm_event *ev) {
    if (ev->type == WM_EV_WINDOW_LIST) {
        int n = (int)ev->code;
        if (n > 8) n = 8;
        g_win_count = n;
        for (int i = 0; i < n; i++) {
            struct wm_window_entry e;
            if (pipe_read(g_panel_ev, &e, sizeof(e)) != sizeof(e)) {
                g_win_count = i;
                break;
            }
            g_win_list[i].id = e.id;
            g_win_list[i].minimized = e.minimized;
            memcpy(g_win_list[i].title, e.title, 32);
            g_win_list[i].title[31] = '\0';
        }
        draw_panel();
        return;
    }

    if (ev->type == WM_EV_MOUSE_DOWN && ev->code == 1) {
        if (ev->x < WIN_ITEM_X0) {
            open_start_menu();
            return;
        }
        if (ev->x >= WIN_ITEM_X0 &&
            ev->x < WIN_ITEM_X0 + WIN_ITEM_W * g_win_count) {
            int idx = (ev->x - WIN_ITEM_X0) / WIN_ITEM_W;
            if (idx >= 0 && idx < g_win_count) {
                struct wm_action_request req;
                memset(&req, 0, sizeof(req));
                req.magic = WM_MAGIC;
                req.type = WM_REQ_WIN_ACTION;
                req.owner_pid = getpid();
                req.win_id = g_win_list[idx].id;
                req.action = g_win_list[idx].minimized
                             ? WM_ACTION_RESTORE
                             : WM_ACTION_MINIMIZE;
                pipe_write(g_inbox_w, &req, sizeof(req));
            }
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    const char *sw = getenv("SCREEN_W");
    const char *sh = getenv("SCREEN_H");
    g_W = sw ? atoi(sw) : 1024;
    g_H = sh ? atoi(sh) : 768;

    for (int i = 0; i < 100; i++) {
        g_inbox_w = pipe_open(WM_INBOX_NAME, 1);
        if (g_inbox_w >= 0) break;
        sleep_ms(10);
    }
    if (g_inbox_w < 0) {
        printf("desktop: wm-inbox open failed\n");
        return 1;
    }

    wm_font_load();

    /* 壁纸 */
    strcpy(g_wall_shm, "wall-shm");
    char ev_wall[24]; strcpy(ev_wall, "wall-ev");
    if (create_wm_window("", WM_KIND_WALLPAPER, 0, 0, g_W, g_H, 0,
                         &g_wall_pixels, &g_wall_hdr,
                         g_wall_shm, ev_wall, &g_wall_ev) < 0) {
        printf("desktop: wallpaper create failed\n");
        return 1;
    }
    draw_wallpaper();

    /* 面板 */
    strcpy(g_panel_shm, "panel-shm");
    char ev_panel[24]; strcpy(ev_panel, "panel-ev");
    if (create_wm_window("", WM_KIND_DOCK, 0, g_H - PANEL_H, g_W, PANEL_H, 0,
                         &g_panel_pixels, &g_panel_hdr,
                         g_panel_shm, ev_panel, &g_panel_ev) < 0) {
        printf("desktop: panel create failed\n");
        return 1;
    }
    draw_panel();
    update_clock();

    int clock_tick = 0;

    for (;;) {
        struct wm_event ev;

        while (g_panel_ev >= 0 &&
               pipe_avail(g_panel_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_panel_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            handle_panel_event(&ev);
        }
        while (g_wall_ev >= 0 &&
               pipe_avail(g_wall_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_wall_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            /* 壁纸第一版不响应 */
        }
        while (g_menu_ev >= 0 &&
               pipe_avail(g_menu_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_menu_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            handle_menu_event(&ev);
        }

        if (++clock_tick >= 500) {
            clock_tick = 0;
            update_clock();
        }

        sleep_ms(1);
    }

    return 0;
}