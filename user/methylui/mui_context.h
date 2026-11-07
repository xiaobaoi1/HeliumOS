#ifndef METHYLUI_CONTEXT_H
#define METHYLUI_CONTEXT_H

#include "methylui/mui_widget.h"
#include "wm_proto.h"

struct mui_context {
    char shm_name[16];
    char ev_name[24];
    int  ev_rd;
    int  inbox_w;

    struct shm_hdr *hdr;
    unsigned int   *pixels;
    int             cw, ch;

    struct mui_surface surface;

    struct mui_widget *root;

    struct mui_widget *hover;
    struct mui_widget *pressed;
    struct mui_widget *focus;

    unsigned int bg_color;

    int close_requested;
    /* 返回 0 允许关闭，非 0 拒绝。NULL 表示总是允许。 */
    int (*on_close)(struct mui_context *ctx);
};

int  mui_context_init(struct mui_context *ctx,
                      const char *title,
                      const char *shm_name,
                      const char *ev_name,
                      int w, int h,
                      int min_w, int min_h,
                      int max_w, int max_h);

void mui_context_destroy(struct mui_context *ctx);

void mui_context_dispatch(struct mui_context *ctx, struct wm_event *ev);
void mui_context_paint(struct mui_context *ctx);

void mui_context_run(struct mui_context *ctx);

#endif