#include "methylui/mui_textbox.h"
#include "string.h"
#include "unicode_font.h"
#include "methylui/mui_font.h"

#define TXT_PAD_X 4

static int utf8_prev_boundary(const char *s, int pos) {
    if (pos <= 0) return 0;
    int p = pos - 1;
    while (p > 0 && ((unsigned char)s[p] & 0xC0) == 0x80) p--;
    return p;
}

static int utf8_next_boundary(const char *s, int pos, int len) {
    if (pos >= len) return len;
    uint32_t cp;
    int n = ufont_utf8_decode((const unsigned char*)s + pos, &cp);
    if (n < 1) n = 1;
    if (pos + n > len) n = len - pos;
    return pos + n;
}

static int text_cells(const char *s, int bytes) {
    int x = 0, i = 0;
    while (i < bytes && s[i]) {
        uint32_t cp;
        int n = ufont_utf8_decode((const unsigned char*)s + i, &cp);
        int w = mui_font_width((int)cp);
        x += w ? w : MUI_FONT_W;
        i += n;
    }
    return x;
}

static void textbox_paint(struct mui_widget *w, struct mui_surface *s,
                          int ax, int ay) {
    struct mui_textbox *t = (struct mui_textbox*)w;

    mui_fill(s, ax, ay, w->w, w->h, t->bg);
    unsigned int bc = w->focused ? t->focus_border : t->border;
    mui_border(s, ax, ay, w->w, w->h, bc);

    int inner = w->w - TXT_PAD_X * 2;
    int tx = ax + TXT_PAD_X;
    int ty = ay + (w->h - MUI_FONT_H) / 2;
    int cx = 0, i = 0;
    while (i < t->len && t->text[i]) {
        uint32_t cp;
        int n = ufont_utf8_decode((const unsigned char*)t->text + i, &cp);
        int cw = mui_font_width((int)cp);
        if (!cw) cw = MUI_FONT_W;
        if (cx + cw > inner) break;
        mui_draw_char(s, tx + cx, ty, cp, t->fg, t->bg);
        cx += cw;
        i += n;
    }

    if (w->focused) {
        int cur = text_cells(t->text, t->cursor);
        if (cur < inner) {
            uint32_t cp = 0;
            int cw = MUI_FONT_W;
            if (t->cursor < t->len)
                ufont_utf8_decode((const unsigned char*)t->text + t->cursor, &cp);
            if (cp) {
                int ww = mui_font_width((int)cp);
                if (ww) cw = ww;
            }
            mui_fill(s, tx + cur, ty + MUI_FONT_H - 2, cw, 2, t->fg);
        }
    }
}

static void textbox_ensure_cursor_visible(struct mui_textbox *t) {
    (void)t; /* Keep the byte stream intact; filenames fit the visible field. */
}

void mui_textbox_move_cursor(struct mui_textbox *t, int delta) {
    if (!t) return;
    int pos = t->cursor;
    if (delta < 0) {
        while (delta++ < 0 && pos > 0)
            pos = utf8_prev_boundary(t->text, pos);
    } else {
        while (delta-- > 0 && pos < t->len)
            pos = utf8_next_boundary(t->text, pos, t->len);
    }
    if (pos == t->cursor) return;
    t->cursor = pos;
    t->base.dirty = 1;
}

void mui_textbox_cursor_home(struct mui_textbox *t) {
    if (!t) return;
    t->cursor = 0;
    t->base.dirty = 1;
}

void mui_textbox_cursor_end(struct mui_textbox *t) {
    if (!t) return;
    t->cursor = t->len;
    t->base.dirty = 1;
}

static void textbox_key(struct mui_widget *w, int key) {
    struct mui_textbox *t = (struct mui_textbox*)w;

    if (key == '\b' || key == 127) {
        if (t->cursor > 0) {
            int prev = utf8_prev_boundary(t->text, t->cursor);
            int n = t->cursor - prev;
            memmove(t->text + prev, t->text + t->cursor,
                    (size_t)(t->len - t->cursor + 1));
            t->len -= n;
            t->cursor = prev;
            t->base.dirty = 1;
        }
        return;
    }

    if (key < 0x20 || key >= 0x7F) return;
    if (t->len >= MUI_TEXTBOX_MAX_LEN) return;

    memmove(t->text + t->cursor + 1, t->text + t->cursor,
            (size_t)(t->len - t->cursor + 1));
    t->text[t->cursor] = (char)key;
    t->len++;
    t->cursor++;
    t->base.dirty = 1;
}

static void textbox_focus_in(struct mui_widget *w) {
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
void mui_textbox_insert_utf8(struct mui_textbox *t, const char *text) {
    if (!t || !text) return;
    int n = (int)strlen(text);
    if (n <= 0 || t->len + n > MUI_TEXTBOX_MAX_LEN) return;
    memmove(t->text + t->cursor + n, t->text + t->cursor,
            (size_t)(t->len - t->cursor + 1));
    memcpy(t->text + t->cursor, text, (size_t)n);
    t->len += n;
    t->cursor += n;
    t->base.dirty = 1;
}
