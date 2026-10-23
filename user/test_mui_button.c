/* test_mui_button — 两个按钮，点击打印，hover 高亮 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "methylui/mui.h"
#include <stdint.h>

struct mui_button {
    struct mui_widget base;
    char label[32];
    void (*on_click)(struct mui_button *b);
};

static void button_paint(struct mui_widget *w, struct mui_surface *s,
                         int ax, int ay) {
    struct mui_button *b = (struct mui_button*)w;
    unsigned int bg;
    if (w->disabled)     bg = 0x444444;
    else if (w->pressed) bg = 0x1A4A75;
    else if (w->hover)   bg = 0x4A7EB5;
    else                 bg = 0x3A6EA5;

    mui_fill(s, ax, ay, w->w, w->h, bg);
    mui_border(s, ax, ay, w->w, w->h, 0x222222);

    int tw = mui_text_width(b->label);
    int tx = ax + (w->w - tw) / 2;
    int ty = ay + (w->h - MUI_FONT_H) / 2;
    mui_text(s, tx, ty, b->label, 0xFFFFFF, bg);
}

static void button_mouse_up(struct mui_widget *w, int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    struct mui_button *b = (struct mui_button*)w;
    if (b->on_click) b->on_click(b);
}

static void button_init(struct mui_button *b, int x, int y, int w, int h,
                        const char *label) {
    memset(b, 0, sizeof(*b));
    mui_widget_init(&b->base, x, y, w, h);
    b->base.on_paint = button_paint;
    b->base.on_mouse_up = button_mouse_up;
    b->base.focusable = 1;
    strncpy(b->label, label, sizeof(b->label) - 1);
}

static void on_ok(struct mui_button *b) {
    (void)b;
    dprintf(2, "[mui] OK clicked\n");
}

static void on_cancel(struct mui_button *b) {
    (void)b;
    dprintf(2, "[mui] Cancel clicked\n");
}

int main(void) {
    struct mui_context ctx;
    if (mui_context_init(&ctx, "MethylUI Button Test",
                         "mui-btn-shm", "mui-btn-ev",
                         300, 200,
                         200, 100, 800, 600) < 0) {
        dprintf(2, "mui_context_init failed\n");
        return 1;
    }

    struct mui_widget root;
    mui_widget_init(&root, 0, 0, 300, 200);
    root.dirty = 1;
    ctx.root = &root;

    struct mui_button ok, cancel;
    button_init(&ok, 30, 80, 100, 32, "OK");
    ok.on_click = on_ok;

    button_init(&cancel, 170, 80, 100, 32, "Cancel");
    cancel.on_click = on_cancel;

    mui_widget_add_child(&root, &ok.base);
    mui_widget_add_child(&root, &cancel.base);

    mui_context_run(&ctx);
    mui_context_destroy(&ctx);
    return 0;
}