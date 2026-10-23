#include "methylui/mui_context.h"
#include "methylui/mui_font.h"
#include "syscall.h"
#include "string.h"
#include <stdint.h>

/* 根容器默认背景绘制 */
static void default_root_paint(struct mui_widget *w, struct mui_surface *s,
                               int ax, int ay) {
    mui_fill(s, ax, ay, w->w, w->h, 0x1A1A2E);
}

static void set_focus(struct mui_context *ctx, struct mui_widget *w) {
    if (ctx->focus == w) return;
    if (ctx->focus) {
        ctx->focus->focused = 0;
        ctx->focus->dirty = 1;
        if (ctx->focus->on_focus_out) ctx->focus->on_focus_out(ctx->focus);
    }
    ctx->focus = w;
    if (w) {
        w->focused = 1;
        w->dirty = 1;
        if (w->on_focus_in) w->on_focus_in(w);
    }
}

int mui_context_init(struct mui_context *ctx,
                     const char *title,
                     const char *shm_name,
                     const char *ev_name,
                     int w, int h,
                     int min_w, int min_h,
                     int max_w, int max_h) {
    if (!ctx) return -1;
    memset(ctx, 0, sizeof(*ctx));

    mui_font_load();

    ctx->ev_rd = -1;
    ctx->inbox_w = -1;
    ctx->cw = w;
    ctx->ch = h;
    ctx->bg_color = 0x1A1A2E;

    strncpy(ctx->shm_name, shm_name, sizeof(ctx->shm_name) - 1);
    strncpy(ctx->ev_name, ev_name, sizeof(ctx->ev_name) - 1);

    int shm_size = shm_total_size(w, h);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) return -1;

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) return -1;

    ctx->hdr = (struct shm_hdr*)addr;
    ctx->pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(ctx->pixels, 0, w * h * 4);
    ctx->hdr->counter = 0;
    ctx->hdr->cur_x0 = SHM_DIRTY_EMPTY; ctx->hdr->cur_y0 = 0;
    ctx->hdr->cur_x1 = 0; ctx->hdr->cur_y1 = 0;
    ctx->hdr->pub_x0 = SHM_DIRTY_EMPTY; ctx->hdr->pub_y0 = 0;
    ctx->hdr->pub_x1 = 0; ctx->hdr->pub_y1 = 0;

    ctx->surface.pixels = ctx->pixels;
    ctx->surface.w = w;
    ctx->surface.h = h;
    ctx->surface.hdr = ctx->hdr;

    pipe_unlink(ev_name);
    int fds[2];
    if (pipe_named(ev_name, fds) < 0) return -1;
    ctx->ev_rd = fds[0];
    ipc_close(fds[1]);

    ctx->inbox_w = pipe_open(WM_INBOX_NAME, 1);
    if (ctx->inbox_w < 0) return -1;

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type  = WM_REQ_CREATE;
    req.kind  = WM_KIND_NORMAL;
    req.x = 0; req.y = 0;
    req.w = w; req.h = h;
    req.min_w = min_w; req.min_h = min_h;
    req.max_w = max_w; req.max_h = max_h;
    req.owner_pid = getpid();
    strncpy(req.shm_name, shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, ev_name, sizeof(req.ev_name) - 1);
    strncpy(req.title, title ? title : "", sizeof(req.title) - 1);
    if (pipe_write(ctx->inbox_w, &req, sizeof(req)) != sizeof(req))
        return -1;

    return 0;
}

void mui_context_destroy(struct mui_context *ctx) {
    if (!ctx) return;

    if (ctx->inbox_w >= 0) {
        struct wm_destroy_request dr;
        dr.magic = WM_MAGIC;
        dr.type  = WM_REQ_DESTROY;
        dr.owner_pid = getpid();
        strncpy(dr.shm_name, ctx->shm_name, sizeof(dr.shm_name) - 1);
        pipe_write(ctx->inbox_w, &dr, sizeof(dr));
    }

    if (ctx->ev_rd >= 0) ipc_close(ctx->ev_rd);
    if (ctx->inbox_w >= 0) ipc_close(ctx->inbox_w);
    if (ctx->hdr)
        munmap((unsigned int)ctx->hdr, shm_total_size(ctx->cw, ctx->ch));

    ctx->ev_rd = -1;
    ctx->inbox_w = -1;
    ctx->hdr = NULL;
    ctx->pixels = NULL;
    ctx->root = NULL;
}

static void dispatch_mouse_move(struct mui_context *ctx, int x, int y) {
    struct mui_widget *hit = mui_widget_hit_test(ctx->root, x, y);

    if (hit != ctx->hover) {
        if (ctx->hover) {
            ctx->hover->hover = 0;
            ctx->hover->dirty = 1;
            if (ctx->hover->on_mouse_leave) ctx->hover->on_mouse_leave(ctx->hover);
        }
        ctx->hover = hit;
        if (hit) {
            hit->hover = 1;
            hit->dirty = 1;
            if (hit->on_mouse_enter) hit->on_mouse_enter(hit);
        }
    }

    if (hit && hit->on_mouse_move) {
        int lx = x - mui_widget_abs_x(hit);
        int ly = y - mui_widget_abs_y(hit);
        hit->on_mouse_move(hit, lx, ly);
    }
}

static void dispatch_mouse_down(struct mui_context *ctx, int x, int y, int btn) {
    struct mui_widget *hit = mui_widget_hit_test(ctx->root, x, y);
    ctx->pressed = hit;

    if (hit) {
        hit->pressed = 1;
        hit->dirty = 1;
        if (hit->focusable) set_focus(ctx, hit);

        int lx = x - mui_widget_abs_x(hit);
        int ly = y - mui_widget_abs_y(hit);
        if (hit->on_mouse_down) hit->on_mouse_down(hit, lx, ly, btn);
    }
}

static void dispatch_mouse_up(struct mui_context *ctx, int x, int y, int btn) {
    struct mui_widget *hit = mui_widget_hit_test(ctx->root, x, y);
    struct mui_widget *pressed = ctx->pressed;
    ctx->pressed = NULL;

    if (pressed) {
        pressed->pressed = 0;
        pressed->dirty = 1;
    }

    if (hit && hit == pressed) {
        int lx = x - mui_widget_abs_x(hit);
        int ly = y - mui_widget_abs_y(hit);
        if (hit->on_mouse_up) hit->on_mouse_up(hit, lx, ly, btn);
    }
}

static void dispatch_key(struct mui_context *ctx, int key) {
    if (ctx->focus && ctx->focus->on_key)
        ctx->focus->on_key(ctx->focus, key);
}

void mui_context_dispatch(struct mui_context *ctx, struct wm_event *ev) {
    switch (ev->type) {
        case WM_EV_MOUSE_MOVE:
            dispatch_mouse_move(ctx, ev->x, ev->y);
            break;
        case WM_EV_MOUSE_DOWN:
            dispatch_mouse_down(ctx, ev->x, ev->y, (int)ev->code);
            break;
        case WM_EV_MOUSE_UP:
            dispatch_mouse_up(ctx, ev->x, ev->y, (int)ev->code);
            break;
        case WM_EV_KEY:
            dispatch_key(ctx, (int)ev->code);
            break;
        case WM_EV_CLOSE_REQUEST:
            ctx->close_requested = 1;
            break;
        case WM_EV_RESIZE:
            /* 第一版不做——忽略。maximize 后客户区左上角保留旧内容。 */
            break;
        default:
            break;
    }
}

void mui_context_paint(struct mui_context *ctx) {
    if (!ctx || !ctx->root) return;

    if (ctx->root->dirty) {
        if (!ctx->root->on_paint)
            ctx->root->on_paint = default_root_paint;
    }

    mui_widget_paint(ctx->root, &ctx->surface, 0, 0);

    if (ctx->hdr->pub_x0 == SHM_DIRTY_EMPTY ||
        ctx->hdr->pub_x1 <= ctx->hdr->pub_x0) {
        if (ctx->hdr->cur_x0 != SHM_DIRTY_EMPTY &&
            ctx->hdr->cur_x1 > ctx->hdr->cur_x0) {
            ctx->hdr->pub_x0 = ctx->hdr->cur_x0;
            ctx->hdr->pub_y0 = ctx->hdr->cur_y0;
            ctx->hdr->pub_x1 = ctx->hdr->cur_x1;
            ctx->hdr->pub_y1 = ctx->hdr->cur_y1;
            ctx->hdr->counter++;
            ctx->hdr->cur_x0 = SHM_DIRTY_EMPTY;
            ctx->hdr->cur_y0 = 0;
            ctx->hdr->cur_x1 = 0;
            ctx->hdr->cur_y1 = 0;
        }
    }
}

void mui_context_run(struct mui_context *ctx) {
    for (;;) {
        struct wm_event ev;
        while (pipe_avail(ctx->ev_rd) >= (int)sizeof(ev)) {
            int n = pipe_read(ctx->ev_rd, &ev, sizeof(ev));
            if (n != sizeof(ev)) break;
            mui_context_dispatch(ctx, &ev);
        }

        if (ctx->close_requested) {
            if (!ctx->on_close || ctx->on_close(ctx) == 0)
                return;
            ctx->close_requested = 0;
        }

        mui_context_paint(ctx);
        sleep_ms(1);
    }
}