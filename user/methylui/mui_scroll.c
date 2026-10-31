#include "methylui/mui_scroll.h"
#include "string.h"

#define SCROLL_MIN_THUMB 12

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int mui_scroll_value(struct mui_scroll *s) {
    return s ? s->value : 0;
}

int mui_scroll_max_value(struct mui_scroll *s) {
    if (!s || s->total <= 0 || s->visible <= 0) return 0;
    int m = s->total - s->visible;
    return m > 0 ? m : 0;
}

int mui_scroll_thumb_h(struct mui_scroll *s) {
    if (!s || s->total <= 0 || s->visible <= 0) return 0;
    if (s->visible >= s->total) return s->base.h;
    int h = s->base.h * s->visible / s->total;
    if (h < SCROLL_MIN_THUMB) h = SCROLL_MIN_THUMB;
    if (h > s->base.h) h = s->base.h;
    return h;
}

int mui_scroll_thumb_y(struct mui_scroll *s, int ay) {
    int max_value = mui_scroll_max_value(s);
    if (max_value <= 0) return ay;
    int thumb_h = mui_scroll_thumb_h(s);
    int range = s->base.h - thumb_h;
    if (range <= 0) return ay;
    return ay + range * s->value / max_value;
}

static void scroll_value_changed(struct mui_scroll *s) {
    s->base.dirty = 1;
    if (s->on_change) s->on_change(s);
}

void mui_scroll_set_range(struct mui_scroll *s, int total, int visible) {
    if (!s) return;
    if (total < 0) total = 0;
    if (visible < 0) visible = 0;
    s->total = total;
    s->visible = visible;
    int max_value = mui_scroll_max_value(s);
    if (s->value > max_value) s->value = max_value;
    s->base.dirty = 1;
}

void mui_scroll_set_value(struct mui_scroll *s, int value) {
    if (!s) return;
    int max_value = mui_scroll_max_value(s);
    value = clamp_int(value, 0, max_value);
    if (value == s->value) return;
    s->value = value;
    scroll_value_changed(s);
}

void mui_scroll_by(struct mui_scroll *s, int rows) {
    if (!s) return;
    mui_scroll_set_value(s, s->value + rows);
}

void mui_scroll_page(struct mui_scroll *s, int dir) {
    if (!s) return;
    int step = s->visible > 1 ? s->visible - 1 : 1;
    mui_scroll_by(s, dir * step);
}

void mui_scroll_home(struct mui_scroll *s) {
    if (!s) return;
    mui_scroll_set_value(s, 0);
}

void mui_scroll_end(struct mui_scroll *s) {
    if (!s) return;
    mui_scroll_set_value(s, mui_scroll_max_value(s));
}

void mui_scroll_ensure_visible(struct mui_scroll *s, int idx) {
    if (!s || s->visible <= 0) return;
    if (idx < 0) idx = 0;
    if (idx < s->value) { mui_scroll_set_value(s, idx); return; }
    if (idx >= s->value + s->visible)
        mui_scroll_set_value(s, idx - s->visible + 1);
}

static void scroll_paint(struct mui_widget *w, struct mui_surface *surf,
                         int ax, int ay) {
    struct mui_scroll *s = (struct mui_scroll *)w;
    mui_fill(surf, ax, ay, w->w, w->h, s->bar_bg);

    if (mui_scroll_max_value(s) <= 0 || s->visible <= 0) return;

    int thumb_h = mui_scroll_thumb_h(s);
    int thumb_y = mui_scroll_thumb_y(s, ay);
    unsigned int tc = s->bar_fg;
    if (s->dragging)   tc = s->bar_drag;
    else if (w->hover) tc = s->bar_hover;

    mui_fill(surf, ax + 1, thumb_y, w->w - 2, thumb_h, tc);
}

static void scroll_mouse_down(struct mui_widget *w, int x, int y, int btn) {
    (void)btn;
    struct mui_scroll *s = (struct mui_scroll *)w;

    /* 清除上一次可能残留的拖动状态（若 mouse_up 在别处释放未被接收） */
    s->dragging = 0;

    if (mui_scroll_max_value(s) <= 0) return;
    if (x < 0 || x >= w->w) return;

    int thumb_h = mui_scroll_thumb_h(s);
    int thumb_y = mui_scroll_thumb_y(s, 0);
    int rel_y = y - thumb_y;

    if (rel_y >= 0 && rel_y < thumb_h) {
        s->dragging = 1;
        s->drag_offset = rel_y;
        w->dirty = 1;
    } else if (y < thumb_y) {
        mui_scroll_page(s, -1);
    } else {
        mui_scroll_page(s, 1);
    }
}

static void scroll_mouse_move(struct mui_widget *w, int x, int y) {
    (void)x;
    struct mui_scroll *s = (struct mui_scroll *)w;
    if (!s->dragging) return;

    int max_value = mui_scroll_max_value(s);
    if (max_value <= 0) return;

    int thumb_h = mui_scroll_thumb_h(s);
    int range = w->h - thumb_h;
    if (range <= 0) return;

    int new_thumb_y = y - s->drag_offset;
    new_thumb_y = clamp_int(new_thumb_y, 0, range);
    int new_value = new_thumb_y * max_value / range;
    mui_scroll_set_value(s, new_value);
}

static void scroll_mouse_up(struct mui_widget *w, int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    struct mui_scroll *s = (struct mui_scroll *)w;
    if (!s->dragging) return;
    s->dragging = 0;
    w->dirty = 1;
}

static void scroll_handle_esc(struct mui_scroll *s) {
    const char *seq = s->esc_buf;
    if (strcmp(seq, "[A") == 0)       mui_scroll_by(s, -1);
    else if (strcmp(seq, "[B") == 0)  mui_scroll_by(s, 1);
    else if (strcmp(seq, "[5~") == 0) mui_scroll_page(s, -1);
    else if (strcmp(seq, "[6~") == 0) mui_scroll_page(s, 1);
    else if (strcmp(seq, "[H") == 0)  mui_scroll_home(s);
    else if (strcmp(seq, "[F") == 0)  mui_scroll_end(s);
}

static void scroll_key(struct mui_widget *w, int key) {
    struct mui_scroll *s = (struct mui_scroll *)w;

    /* ESC 序列解析：与 desktop.c 的 fm_handle_key 风格一致 */
    if (s->esc_state) {
        if (s->esc_state == 1) {
            if (key == '[') {
                s->esc_state = 2;
                s->esc_len = 1;
                s->esc_buf[0] = '[';
                s->esc_buf[1] = '\0';
                return;
            }
            s->esc_state = 0;
            return;
        }
        if (s->esc_len < (int)sizeof(s->esc_buf) - 1)
            s->esc_buf[s->esc_len++] = (char)key;
        s->esc_buf[s->esc_len] = '\0';
        if (key == '~' || (key >= 'A' && key <= 'Z')) {
            s->esc_state = 0;
            scroll_handle_esc(s);
        }
        return;
    }

    if (key == 27) { s->esc_state = 1; s->esc_len = 0; return; }

    /* 无方向键序列时，兼容 vi 风格 j/k/u/d */
    if      (key == 'k' || key == 'K') mui_scroll_by(s, -1);
    else if (key == 'j' || key == 'J') mui_scroll_by(s, 1);
    else if (key == 'u' || key == 'U') mui_scroll_page(s, -1);
    else if (key == 'd' || key == 'D') mui_scroll_page(s, 1);
}

void mui_scroll_init(struct mui_scroll *s, int x, int y, int w, int h) {
    memset(s, 0, sizeof(*s));
    if (w < 4) w = 4;
    if (h < 4) h = 4;
    mui_widget_init(&s->base, x, y, w, h);
    s->base.on_paint      = scroll_paint;
    s->base.on_mouse_down = scroll_mouse_down;
    s->base.on_mouse_move = scroll_mouse_move;
    s->base.on_mouse_up   = scroll_mouse_up;
    s->base.on_key        = scroll_key;
    s->base.focusable     = 1;

    s->bar_bg    = 0x14141A;   /* 与 mui_list 的 bar_bg 一致 */
    s->bar_fg    = 0x5A5A7E;   /* 与 mui_list 的 bar_fg 一致 */
    s->bar_hover = 0x6A6A8E;
    s->bar_drag  = 0x8A8AAE;
}