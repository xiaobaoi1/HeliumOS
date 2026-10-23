/* test_mui_form — label + textbox + button 组合 */
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
    if (w->pressed) bg = 0x1A4A75;
    else if (w->hover) bg = 0x4A7EB5;
    else bg = 0x3A6EA5;

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

static struct mui_textbox *g_name_box;
static struct mui_textbox *g_pass_box;

static void on_login(struct mui_button *b) {
    (void)b;
    dprintf(2, "[form] login: name='%s' pass='%s'\n",
            mui_textbox_get_text(g_name_box),
            mui_textbox_get_text(g_pass_box));
}

int main(void) {
    struct mui_context ctx;
    if (mui_context_init(&ctx, "MethylUI Form Test",
                         "mui-form-shm", "mui-form-ev",
                         400, 280,
                         300, 200, 800, 600) < 0) {
        dprintf(2, "mui_context_init failed\n");
        return 1;
    }

    struct mui_widget root;
    mui_widget_init(&root, 0, 0, 400, 280);
    root.dirty = 1;
    ctx.root = &root;

    struct mui_label l_name, l_pass;
    mui_label_init(&l_name, 20, 20, "Username:", 0xFFFFFF, 0x1A1A2E);
    mui_label_init(&l_pass, 20, 70, "Password:", 0xFFFFFF, 0x1A1A2E);

    struct mui_textbox name_box, pass_box;
    mui_textbox_init(&name_box, 20, 40, 360);
    mui_textbox_init(&pass_box, 20, 90, 360);
    g_name_box = &name_box;
    g_pass_box = &pass_box;

    struct mui_button login;
    button_init(&login, 20, 150, 120, 32, "Login");
    login.on_click = on_login;

    mui_widget_add_child(&root, &l_name.base);
    mui_widget_add_child(&root, &l_pass.base);
    mui_widget_add_child(&root, &name_box.base);
    mui_widget_add_child(&root, &pass_box.base);
    mui_widget_add_child(&root, &login.base);

    mui_context_run(&ctx);
    mui_context_destroy(&ctx);
    return 0;
}