#include "methylui/mui_label.h"
#include "string.h"

static void label_paint(struct mui_widget *w, struct mui_surface *s,
                        int ax, int ay) {
    struct mui_label *l = (struct mui_label*)w;
    mui_text(s, ax, ay, l->text, l->fg, l->bg);
}

void mui_label_init(struct mui_label *l, int x, int y,
                    const char *text, unsigned int fg, unsigned int bg) {
    memset(l, 0, sizeof(*l));
    int tw = mui_text_width(text);
    mui_widget_init(&l->base, x, y, tw, MUI_FONT_H);
    l->base.on_paint = label_paint;
    strncpy(l->text, text, sizeof(l->text) - 1);
    l->fg = fg;
    l->bg = bg;
}

void mui_label_set_text(struct mui_label *l, const char *text) {
    strncpy(l->text, text, sizeof(l->text) - 1);
    l->text[sizeof(l->text) - 1] = '\0';
    l->base.w = mui_text_width(l->text);
    l->base.dirty = 1;
}