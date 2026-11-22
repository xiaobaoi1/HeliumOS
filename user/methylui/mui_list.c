#include "methylui/mui_list.h"
#include "string.h"
#include "unicode_font.h"
#include "methylui/mui_font.h"

#define LIST_ICON_W 16
#define LIST_TEXT_X 24

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int list_max_top(struct mui_list *l) {
    int visible = mui_list_visible_rows(l);
    int max_top = l->count - visible;
    if (max_top < 0) max_top = 0;
    return max_top;
}

void mui_list_init(struct mui_list *l, int x, int y, int w, int h, int row_h) {
    memset(l, 0, sizeof(*l));
    if (row_h < 8) row_h = 8;
    mui_widget_init(&l->base, x, y, w, h);
    l->base.on_paint = NULL;       /* 使用 mui_list_paint 直接绘制 */
    l->row_h = row_h;
    l->cursor = -1;
    l->hover = -1;
    l->scrollbar_w = 8;
    l->bg = 0x1A1A1E;
    l->fg = 0xFFFFFF;
    l->sel_bg = 0x3A6EA5;
    l->cur_bg = 0x2A3E5A;
    l->hover_bg = 0x24243A;
    l->bar_bg = 0x14141A;
    l->bar_fg = 0x5A5A7E;
}

void mui_list_clear(struct mui_list *l) {
    l->count = 0;
    l->top = 0;
    l->cursor = -1;
    l->hover = -1;
    memset(l->selected, 0, sizeof(l->selected));
    l->base.dirty = 1;
}

void mui_list_set_item(struct mui_list *l, int idx, const char *text, int icon) {
    if (idx < 0 || idx >= MUI_LIST_MAX_ITEMS) return;
    strncpy(l->items[idx].text, text ? text : "", MUI_LIST_TEXT_MAX - 1);
    l->items[idx].text[MUI_LIST_TEXT_MAX - 1] = '\0';
    l->items[idx].icon = (unsigned char)icon;
    if (idx >= l->count) l->count = idx + 1;
    l->base.dirty = 1;
}

void mui_list_select_only(struct mui_list *l, int idx) {
    memset(l->selected, 0, sizeof(l->selected));
    if (idx >= 0 && idx < l->count) {
        l->selected[idx] = 1;
        l->cursor = idx;
        mui_list_ensure_visible(l, idx);
    } else {
        l->cursor = -1;
    }
    l->base.dirty = 1;
}

void mui_list_toggle(struct mui_list *l, int idx) {
    if (idx < 0 || idx >= l->count) return;
    l->selected[idx] = l->selected[idx] ? 0 : 1;
    l->cursor = idx;
    mui_list_ensure_visible(l, idx);
    l->base.dirty = 1;
}

void mui_list_clear_selection(struct mui_list *l) {
    memset(l->selected, 0, sizeof(l->selected));
    l->base.dirty = 1;
}

int mui_list_selected_count(struct mui_list *l) {
    int n = 0;
    for (int i = 0; i < l->count; i++)
        if (l->selected[i]) n++;
    return n;
}

int mui_list_is_selected(struct mui_list *l, int idx) {
    if (idx < 0 || idx >= l->count) return 0;
    return l->selected[idx];
}

int mui_list_visible_rows(struct mui_list *l) {
    if (!l || l->row_h <= 0) return 0;
    return l->base.h / l->row_h;
}

int mui_list_hit(struct mui_list *l, int lx, int ly) {
    if (!l || lx < 0 || ly < 0 || lx >= l->base.w || ly >= l->base.h) return -1;
    if (l->scrollbar_w > 0 && lx >= l->base.w - l->scrollbar_w) return -1;
    int row = l->top + ly / l->row_h;
    if (row < 0 || row >= l->count) return -1;
    return row;
}

void mui_list_ensure_visible(struct mui_list *l, int idx) {
    int visible = mui_list_visible_rows(l);
    if (visible <= 0) { l->top = 0; return; }
    if (idx < l->top) l->top = idx;
    if (idx >= l->top + visible) l->top = idx - visible + 1;
    l->top = clamp_int(l->top, 0, list_max_top(l));
    l->base.dirty = 1;
}

void mui_list_scroll(struct mui_list *l, int rows) {
    l->top = clamp_int(l->top + rows, 0, list_max_top(l));
    l->base.dirty = 1;
}

void mui_list_page(struct mui_list *l, int dir) {
    int visible = mui_list_visible_rows(l);
    int step = visible > 1 ? visible - 1 : 1;
    mui_list_scroll(l, dir * step);
}

static void list_paint_row(struct mui_list *l, struct mui_surface *s,
                           int ax, int y, int idx) {
    int w = l->base.w;
    unsigned int bg = l->bg;
    if (l->selected[idx]) bg = l->sel_bg;
    else if (idx == l->cursor) bg = l->cur_bg;
    else if (idx == l->hover) bg = l->hover_bg;

    mui_fill(s, ax, y, w - l->scrollbar_w, l->row_h, bg);

    int iy = y + (l->row_h - LIST_ICON_W) / 2;
    if (iy < y) iy = y;
    int ix = ax + 4;
    if (l->paint_icon) {
        l->paint_icon(s, ix, iy, l->items[idx].icon);
    } else {
        unsigned int ic = (l->items[idx].icon == MUI_LIST_ICON_DIR) ? 0xE0C060 : 0xC0C0C0;
        mui_fill(s, ix + 2, iy + 3, 12, 10, ic);
        mui_fill(s, ix + 2, iy + 2, 5, 3, ic);
    }

    int text_w = w - l->scrollbar_w - LIST_TEXT_X - 4;
    if (text_w < MUI_FONT_W) text_w = MUI_FONT_W;
    int ty = y + (l->row_h - MUI_FONT_H) / 2;
    int tx = ax + LIST_TEXT_X;
    int limit_x = tx + text_w;
    const unsigned char *p = (const unsigned char*)l->items[idx].text;
    while (*p) {
        uint32_t cp;
        int n = ufont_utf8_decode(p, &cp);
        int cw = mui_font_width((int)cp);
        if (!cw) cw = MUI_FONT_W;
        if (tx + cw > limit_x) break;
        mui_draw_char(s, tx, ty, cp, l->fg, bg);
        tx += cw;
        p += n;
    }
}

void mui_list_paint_row(struct mui_list *l, struct mui_surface *s,
                        int ax, int ay, int idx) {
    if (!l || !s) return;
    if (idx < 0 || idx >= l->count) return;

    int visible = mui_list_visible_rows(l);
    int i = idx - l->top;
    if (i < 0 || i >= visible) return;

    list_paint_row(l, s, ax, ay + i * l->row_h, idx);
}

void mui_list_paint(struct mui_list *l, struct mui_surface *s, int ax, int ay) {
    if (!l || !s) return;

    int w = l->base.w;
    int h = l->base.h;
    int visible = mui_list_visible_rows(l);

    mui_fill(s, ax, ay, w, h, l->bg);

    for (int i = 0; i < visible; i++) {
        int idx = l->top + i;
        if (idx >= l->count) break;
        list_paint_row(l, s, ax, ay + i * l->row_h, idx);
    }

    /* 滚动条 */
    if (l->scrollbar_w > 0 && l->count > visible && visible > 0) {
        int bx = ax + w - l->scrollbar_w;
        mui_fill(s, bx, ay, l->scrollbar_w, h, l->bar_bg);

        int thumb_h = h * visible / l->count;
        if (thumb_h < 12) thumb_h = 12;
        if (thumb_h > h) thumb_h = h;

        int max_top = l->count - visible;
        int thumb_y = ay;
        if (max_top > 0)
            thumb_y += (h - thumb_h) * l->top / max_top;

        mui_fill(s, bx + 1, thumb_y, l->scrollbar_w - 2, thumb_h, l->bar_fg);
    }
}
