#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include "wm_proto.h"
#include "shm_layout.h"
#include "image.h"

static unsigned int *g_pixels;
static struct shm_hdr *g_hdr;
static char g_shm_name[20], g_ev_name[28];
static int g_ev = -1, g_inbox = -1;
static int g_cw = 640, g_ch = 480, g_map_size;

static void publish_all(void) {
    if (!g_hdr) return;
    g_hdr->pub_x0 = 0; g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = (uint32_t)g_cw; g_hdr->pub_y1 = (uint32_t)g_ch;
    g_hdr->counter++;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY; g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0; g_hdr->cur_y1 = 0;
}

static int create_window(const char *title) {
    g_map_size = shm_total_size(g_cw, g_ch);
    int shm_id = shm_open(g_shm_name, g_map_size, 0);
    if (shm_id < 0) return -1;
    unsigned int addr = mmap(0, g_map_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id, 0);
    if ((int)addr <= 0) return -1;
    g_hdr = (struct shm_hdr*)addr;
    g_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(g_pixels, 0, (size_t)g_cw * g_ch * 4);
    g_hdr->counter = 0;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY; g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0; g_hdr->cur_y1 = 0;
    g_hdr->pub_x0 = SHM_DIRTY_EMPTY; g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = 0; g_hdr->pub_y1 = 0;

    pipe_unlink(g_ev_name);
    int fds[2];
    if (pipe_named(g_ev_name, fds) < 0) return -1;
    g_ev = fds[0]; ipc_close(fds[1]);

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC; req.type = WM_REQ_CREATE; req.kind = WM_KIND_NORMAL;
    req.x = 80; req.y = 50; req.w = g_cw; req.h = g_ch;
    req.min_w = 240; req.min_h = 180; req.max_w = 0; req.max_h = 0;
    req.owner_pid = getpid();
    strncpy(req.shm_name, g_shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, g_ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, title, sizeof(req.title) - 1);
    if (pipe_write(g_inbox, &req, sizeof(req)) != sizeof(req)) return -1;
    return 0;
}

static void render(struct image *img) {
    for (int i = 0; i < g_cw * g_ch; i++) g_pixels[i] = 0x12131A;
    if (img && img->pixels && img->w > 0 && img->h > 0) {
        int dw = g_cw, dh = img->h * g_cw / img->w;
        if (dh > g_ch) { dh = g_ch; dw = img->w * g_ch / img->h; }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        int dx = (g_cw - dw) / 2, dy = (g_ch - dh) / 2;
        for (int y = 0; y < dh; y++) {
            int sy = y * img->h / dh;
            for (int x = 0; x < dw; x++) {
                int sx = x * img->w / dw;
                g_pixels[(dy + y) * g_cw + dx + x] =
                    img->pixels[(size_t)sy * img->w + sx] & 0xFFFFFFu;
            }
        }
    }
    publish_all();
}

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    for (int i = 0; i < 2000; i++) {
        g_inbox = pipe_open(WM_INBOX_NAME, 1);
        if (g_inbox >= 0) break;
        sleep_ms(5);
    }
    if (g_inbox < 0) return 1;

    struct image img;
    memset(&img, 0, sizeof(img));
    int ok = image_load(&img, argv[1]);

    if (ok && img.w > 0 && img.h > 0) {
        int maxw = 900, maxh = 620;
        int cw = img.w, ch = img.h;
        if (cw > maxw) { ch = ch * maxw / cw; cw = maxw; }
        if (ch > maxh) { cw = cw * maxh / ch; ch = maxh; }
        if (cw < 320) cw = 320;
        if (ch < 240) ch = 240;
        g_cw = cw; g_ch = ch;
    }

    int pid = getpid();
    snprintf(g_shm_name, sizeof(g_shm_name), "photos-%d", pid);
    snprintf(g_ev_name, sizeof(g_ev_name), "photos-ev-%d", pid);

    const char *base = argv[1];
    for (const char *p = argv[1]; *p; p++) if (*p == '\\') base = p + 1;
    char title[64];
    snprintf(title, sizeof(title), "Photos - %s", base);
    if (create_window(title) < 0) return 1;
    if (argc >= 3) {
        int a = atoi(argv[2]);
        if (a < 0) a = 0;
        if (a > 255) a = 255;
        struct wm_set_alpha_request ar;
        memset(&ar, 0, sizeof(ar));
        ar.magic = WM_MAGIC; ar.type = WM_REQ_SET_ALPHA;
        ar.owner_pid = getpid(); ar.alpha = (uint32_t)a;
        strncpy(ar.shm_name, g_shm_name, sizeof(ar.shm_name) - 1);
        pipe_write(g_inbox, &ar, sizeof(ar));
    }
    render(ok ? &img : NULL);

    int done = 0;
    while (!done) {
        struct wm_event ev;
        while (g_ev >= 0 && pipe_avail(g_ev) >= (int)sizeof(ev)) {
            if (pipe_read(g_ev, &ev, sizeof(ev)) != sizeof(ev)) break;
            if (ev.type == WM_EV_CLOSE_REQUEST || ev.type == WM_EV_CLOSE ||
                (ev.type == WM_EV_KEY && ev.code == 27)) {
                done = 1;
                break;
            }
        }
        if (!done) sleep_ms(1);
    }

    struct wm_destroy_request dr;
    memset(&dr, 0, sizeof(dr));
    dr.magic = WM_MAGIC; dr.type = WM_REQ_DESTROY; dr.owner_pid = getpid();
    strncpy(dr.shm_name, g_shm_name, sizeof(dr.shm_name) - 1);
    if (g_inbox >= 0) pipe_write(g_inbox, &dr, sizeof(dr));
    if (g_hdr) munmap((unsigned int)g_hdr, g_map_size);
    if (g_ev >= 0) ipc_close(g_ev);
    if (ok) image_free(&img);
    return 0;
}
