#ifndef DESKTOP_WM_H
#define DESKTOP_WM_H

#include "syscall.h"
#include "string.h"
#include "wm_proto.h"
#include "shm_layout.h"

extern int g_inbox_w;

static int wm_create_window(const char *title, int kind, int x, int y, int w, int h,
                            unsigned int **out_pixels, struct shm_hdr **out_hdr,
                            char *out_shm_name, char *out_ev_name, int *out_ev_rd) {
    int shm_size = shm_total_size(w, h);
    int shm_id = shm_open(out_shm_name, shm_size, 0);
    if (shm_id < 0) return -1;

    unsigned int addr = mmap(0, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
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
    req.x = x; req.y = y;
    req.w = w; req.h = h;
    req.owner_pid = getpid();
    strncpy(req.shm_name, out_shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, out_ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, title, sizeof(req.title) - 1);

    if (pipe_write(g_inbox_w, &req, sizeof(req)) != sizeof(req)) return -1;

    *out_pixels = pixels;
    *out_hdr = hdr;
    return 0;
}
#endif