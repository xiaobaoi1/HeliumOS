#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include "wm_proto.h"
#include "wm_font.h"
#include "unicode_font.h"
#include "shm_layout.h"

#define ABOUT_W  460
#define ABOUT_H  340

#define COL_BG       0x1A1A2E
#define COL_HEADER   0x3A6EA5
#define COL_TEXT     0xE8E8E8
#define COL_DIM      0x8899AA
#define COL_ACCENT   0x6AA2E8
#define COL_DIVIDER  0x3A4A5E

int g_inbox_w = -1;

static unsigned int *g_pixels;
static struct shm_hdr *g_hdr;
static char  g_shm_name[16];
static char  g_ev_name[24];
static int   g_ev = -1;
static int   g_cw = ABOUT_W;
static int   g_ch = ABOUT_H;
static int   g_map_size = 0;


static void fill_rect(int x, int y, int w, int h, unsigned int c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= g_ch) continue;
        unsigned int *row = g_pixels + py * g_cw;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px < 0 || px >= g_cw) continue;
            row[px] = c;
        }
    }
}

static void draw_text(int x, int y, const char *s, unsigned int fg, unsigned int bg) {
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp;
        int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = 8;
        for (int r = 0; r < 16; r++) {
            int py = y + r;
            if (py < 0 || py >= g_ch) continue;
            for (int j = 0; j < gw; j++) {
                int px = x + j;
                if (px < 0 || px >= g_cw) continue;
                int bit;
                if (gw == 16) bit = g ? (g[r * 2 + (j >> 3)] & (0x80 >> (j & 7))) : 0;
                else          bit = g ? (g[r] & (0x80 >> j)) : 0;
                g_pixels[py * g_cw + px] = bit ? fg : bg;
            }
        }
        x += gw;
        p += n;
    }
}

static void shm_publish_all(void) {
    if (!g_hdr) return;
    g_hdr->pub_x0 = 0;
    g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = (uint32_t)g_cw;
    g_hdr->pub_y1 = (uint32_t)g_ch;
    g_hdr->counter++;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY;
    g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0;
    g_hdr->cur_y1 = 0;
}


static int create_about_window(void) {
    g_map_size = shm_total_size(g_cw, g_ch);
    int shm_id = shm_open(g_shm_name, g_map_size, 0);
    if (shm_id < 0) return -1;

    unsigned int addr = mmap(0, g_map_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) return -1;

    g_hdr = (struct shm_hdr*)addr;
    g_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(g_pixels, 0, g_cw * g_ch * 4);

    g_hdr->counter = 0;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY; g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0;               g_hdr->cur_y1 = 0;
    g_hdr->pub_x0 = SHM_DIRTY_EMPTY; g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = 0;               g_hdr->pub_y1 = 0;

    pipe_unlink(g_ev_name);
    int fds[2];
    if (pipe_named(g_ev_name, fds) < 0) return -1;
    g_ev = fds[0];
    ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type  = WM_REQ_CREATE;
    req.kind  = WM_KIND_NORMAL;
    req.x     = 280;
    req.y     = 180;
    req.w     = g_cw;
    req.h     = g_ch;
    req.min_w = 260;
    req.min_h = 200;
    req.max_w = 0;
    req.max_h = 0;
    req.owner_pid = getpid();
    strncpy(req.shm_name, g_shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, g_ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, "关于 - Helium OS", sizeof(req.title) - 1);

    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return -1;
    return 0;
}


static void render_about(void) {
    fill_rect(0, 0, g_cw, g_ch, COL_BG);

    fill_rect(0, 0, g_cw, 52, COL_HEADER);
    draw_text(20, 18, "关于 Helium 操作系统", 0xFFFFFF, COL_HEADER);

    int y = 76;
    draw_text(24, y, "HeliumOS",        COL_TEXT, COL_BG); y += 28;
    draw_text(24, y, "user app version:   261101-1428", COL_TEXT, COL_BG); y += 24;
    draw_text(24, y, "kernel version:   Helium Kernel (x86, 32-bit)", COL_TEXT, COL_BG); y += 24;

    fill_rect(24, y, g_cw - 48, 1, COL_DIVIDER); y += 14;

    draw_text(24, y, "开发者团队：xiaobaoi1,WBX110822",
              COL_ACCENT, COL_BG);

    shm_publish_all();
}


static void request_destroy_window(void) {
    struct wm_destroy_request dr;
    memset(&dr, 0, sizeof(dr));
    dr.magic = WM_MAGIC;
    dr.type  = WM_REQ_DESTROY;
    dr.owner_pid = getpid();
    strncpy(dr.shm_name, g_shm_name, sizeof(dr.shm_name) - 1);
    if (g_inbox_w >= 0)
        pipe_write(g_inbox_w, &dr, sizeof(dr));
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    for (int i = 0; i < 2000; i++) {
        g_inbox_w = pipe_open(WM_INBOX_NAME, 1);
        if (g_inbox_w >= 0) break;
        sleep_ms(5);
    }
    if (g_inbox_w < 0) {
        printf("about: wm-inbox open failed\n");
        return 1;
    }

    wm_font_load();

    int pid = getpid();
    snprintf(g_shm_name, sizeof(g_shm_name), "about-%d", pid);
    snprintf(g_ev_name,  sizeof(g_ev_name),  "about-ev-%d", pid);

    if (create_about_window() < 0) {
        printf("about: create window failed\n");
        return 1;
    }
    render_about();

    int done = 0;
    while (!done) {
        struct wm_event ev;
        while (g_ev >= 0 && pipe_avail(g_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_ev, &ev, sizeof(ev)) != sizeof(ev)) break;

            if (ev.type == WM_EV_CLOSE_REQUEST || ev.type == WM_EV_CLOSE) {
                done = 1;
                break;
            }
            if (ev.type == WM_EV_KEY && ev.code == 27) {   /* ESC */
                done = 1;
                break;
            }
        }
        if (!done) sleep_ms(1);
    }

    request_destroy_window();

    if (g_hdr) munmap((unsigned int)g_hdr, g_map_size);
    if (g_ev >= 0) ipc_close(g_ev);
    return 0;
}