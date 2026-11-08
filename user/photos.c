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

static struct image_anim g_anim;
static int g_anim_ok = 0;
static int g_frame = 0;
static int g_next_ms = 0;
static int g_elapsed = 0;

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

static void render_frame(int idx) {
    for (int i = 0; i < g_cw * g_ch; i++) g_pixels[i] = 0x12131A;

    if (g_anim_ok && idx >= 0 && idx < g_anim.count) {
        uint32_t *src = g_anim.frames[idx].pixels;
        int iw = g_anim.w, ih = g_anim.h;
        if (src && iw > 0 && ih > 0) {
            int dw = g_cw, dh = ih * g_cw / iw;
            if (dh > g_ch) { dh = g_ch; dw = iw * g_ch / ih; }
            if (dw < 1) dw = 1;
            if (dh < 1) dh = 1;
            int dx = (g_cw - dw) / 2, dy = (g_ch - dh) / 2;
            for (int y = 0; y < dh; y++) {
                int sy = y * ih / dh;
                for (int x = 0; x < dw; x++) {
                    int sx = x * iw / dw;
                    unsigned int c = src[(size_t)sy * iw + sx];
                    if ((c >> 24) == 0) continue;     /* 透明：保留背景 */
                    g_pixels[(dy + y) * g_cw + dx + x] = c & 0xFFFFFFu;
                }
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

    memset(&g_anim, 0, sizeof(g_anim));
    g_anim_ok = image_load_anim(&g_anim, argv[1]);

    if (g_anim_ok && g_anim.w > 0 && g_anim.h > 0) {
        int maxw = 900, maxh = 620;
        int cw = g_anim.w, ch = g_anim.h;
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
    if (create_window(title) < 0) {
        if (g_anim_ok) image_anim_free(&g_anim);
        return 1;
    }

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

    g_frame = 0;
    render_frame(0);
    if (g_anim_ok && g_anim.count > 1) {
        int d = g_anim.frames[0].delay_ms;
        if (d <= 0) d = 100;
        g_next_ms = d;
    } else {
        g_next_ms = 0;
    }

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
        if (done) break;

        if (g_anim_ok && g_anim.count > 1) {
            g_elapsed += 5;
            if (g_elapsed >= g_next_ms) {
                g_elapsed = 0;
                g_frame = (g_frame + 1) % g_anim.count;
                int d = g_anim.frames[g_frame].delay_ms;
                if (d <= 0) d = 100;
                g_next_ms = d;
                render_frame(g_frame);
            }
        }
        sleep_ms(5);
    }

    struct wm_destroy_request dr;
    memset(&dr, 0, sizeof(dr));
    dr.magic = WM_MAGIC; dr.type = WM_REQ_DESTROY; dr.owner_pid = getpid();
    strncpy(dr.shm_name, g_shm_name, sizeof(dr.shm_name) - 1);
    if (g_inbox >= 0) pipe_write(g_inbox, &dr, sizeof(dr));

    if (g_hdr) munmap((unsigned int)g_hdr, g_map_size);
    if (g_ev >= 0) ipc_close(g_ev);
    if (g_anim_ok) image_anim_free(&g_anim);
    return 0;
}