#include "methylui/mui_textbox.h"
#include "string.h"

#define TXT_PAD_X 4

static void textbox_paint(struct mui_widget *w, struct mui_surface *s,
                          int ax, int ay) {
    struct mui_textbox *t = (struct mui_textbox*)w;

    mui_fill(s, ax, ay, w->w, w->h, t->bg);

    unsigned int bc = w->focused ? t->focus_border : t->border;
    mui_border(s, ax, ay, w->w, w->h, bc);

    /* 可见字符数 */
    int visible = (w->w - TXT_PAD_X * 2) / MUI_FONT_W;
    int tx = ax + TXT_PAD_X;
    int ty = ay + (w->h - MUI_FONT_H) / 2;

    for (int i = 0; i < visible && i + t->scroll < t->len; i++) {
        int ch = (unsigned char)t->text[i + t->scroll];
        mui_draw_char(s, tx + i * MUI_FONT_W, ty, ch, t->fg, t->bg);
    }

    /* 光标 */
    if (w->focused) {
        int ci = t->cursor - t->scroll;
        if (ci >= 0 && ci < visible) {
            int cx = tx + ci * MUI_FONT_W;
            /* 下划线 */
            mui_fill(s, cx, ty + MUI_FONT_H - 2, MUI_FONT_W, 2, t->fg);
        }
    }
}

static void textbox_ensure_cursor_visible(struct mui_textbox *t) {
    int visible = (t->base.w - TXT_PAD_X * 2) / MUI_FONT_W;
    if (visible <= 0) visible = 1;
    if (t->cursor < t->scroll) {
        t->scroll = t->cursor;
        t->base.dirty = 1;
    } else if (t->cursor >= t->scroll + visible) {
        t->scroll = t->cursor - visible + 1;
        t->base.dirty = 1;
    }
}

void mui_textbox_move_cursor(struct mui_textbox *t, int delta) {
    if (!t) return;
    int pos = t->cursor + delta;
    if (pos < 0) pos = 0;
    if (pos > t->len) pos = t->len;
    if (pos == t->cursor) return;
    t->cursor = pos;
    t->base.dirty = 1;
    textbox_ensure_cursor_visible(t);
}

void mui_textbox_cursor_home(struct mui_textbox *t) {
    if (!t) return;
    mui_textbox_move_cursor(t, -t->cursor);
}

void mui_textbox_cursor_end(struct mui_textbox *t) {
    if (!t) return;
    mui_textbox_move_cursor(t, t->len - t->cursor);
}

static void textbox_key(struct mui_widget *w, int key) {
    struct mui_textbox *t = (struct mui_textbox*)w;

    if (key == '\b' || key == 127) {
        if (t->cursor > 0) {
            for (int i = t->cursor - 1; i < t->len; i++)
                t->text[i] = t->text[i + 1];
            t->len--;
            t->cursor--;
            t->base.dirty = 1;
            textbox_ensure_cursor_visible(t);
        }
        return;
    }

    if (key < 0x20 || key >= 0x7F) return;
    if (t->len >= MUI_TEXTBOX_MAX_LEN) return;

    for (int i = t->len; i > t->cursor; i--)
        t->text[i] = t->text[i - 1];
    t->text[t->cursor] = (char)key;
    t->len++;
    t->cursor++;
    t->base.dirty = 1;
    textbox_ensure_cursor_visible(t);
}

static void textbox_focus_in(struct mui_widget *w) {
    struct mui_textbox *t = (struct mui_textbox*)w;
    t->scroll = 0;
    textbox_ensure_cursor_visible(t);
    w->dirty = 1;
}

void mui_textbox_init(struct mui_textbox *t, int x, int y, int w) {
    memset(t, 0, sizeof(*t));
    mui_widget_init(&t->base, x, y, w, MUI_FONT_H + 8);
    t->base.on_paint = textbox_paint;
    t->base.on_key = textbox_key;
    t->base.on_focus_in = textbox_focus_in;
    t->base.focusable = 1;
    t->bg = 0x2A2A3E;
    t->fg = 0xFFFFFF;
    t->border = 0x555555;
    t->focus_border = 0x3A6EA5;
}

const char *mui_textbox_get_text(struct mui_textbox *t) {
    return t->text;
}

void mui_textbox_set_text(struct mui_textbox *t, const char *text) {
    if (!text) text = "";
    t->len = strlen(text);
    if (t->len > MUI_TEXTBOX_MAX_LEN) t->len = MUI_TEXTBOX_MAX_LEN;
    memcpy(t->text, text, t->len);
    t->text[t->len] = '\0';
    t->cursor = t->len;
    t->scroll = 0;
    t->base.dirty = 1;
    textbox_ensure_cursor_visible(t);
}