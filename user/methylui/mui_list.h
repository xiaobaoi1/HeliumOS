#ifndef METHYLUI_LIST_H
#define METHYLUI_LIST_H

#include "methylui/mui_widget.h"

#define MUI_LIST_MAX_ITEMS 128
#define MUI_LIST_TEXT_MAX  64

#define MUI_LIST_ICON_FILE 0
#define MUI_LIST_ICON_DIR  1
#define MUI_LIST_ICON_EXEC 2

struct mui_list_item {
    char text[MUI_LIST_TEXT_MAX];
    unsigned char icon;
};

/* 滚动列表控件：一行一项，支持单选/多选、滚动条、鼠标悬浮。
 * 使用方负责用 mui_list_hit / mui_list_scroll 等处理事件，
 * 然后调用 mui_list_paint（或通过 mui_widget_paint）绘制。 */
struct mui_list {
    struct mui_widget base;

    struct mui_list_item items[MUI_LIST_MAX_ITEMS];
    int count;

    int row_h;              /* 行高（像素） */
    int top;                /* 首个可见行 */
    int cursor;             /* 当前行（-1 = 无） */
    int hover;              /* 悬停行（-1 = 无） */
    int scrollbar_w;        /* 右侧滚动条宽度，0 = 不绘制 */

    unsigned char selected[MUI_LIST_MAX_ITEMS];

    unsigned int bg, fg, sel_bg, cur_bg, hover_bg;
    unsigned int bar_bg, bar_fg;

    /* 可选：使用方提供图标绘制。为 NULL 时画一个简单色块。 */
    void (*paint_icon)(struct mui_surface *s, int x, int y, int icon);
};

void mui_list_init(struct mui_list *l, int x, int y, int w, int h, int row_h);

void mui_list_clear(struct mui_list *l);
void mui_list_set_item(struct mui_list *l, int idx, const char *text, int icon);

void mui_list_select_only(struct mui_list *l, int idx);
void mui_list_toggle(struct mui_list *l, int idx);
void mui_list_clear_selection(struct mui_list *l);
int  mui_list_selected_count(struct mui_list *l);
int  mui_list_is_selected(struct mui_list *l, int idx);

int  mui_list_visible_rows(struct mui_list *l);
int  mui_list_hit(struct mui_list *l, int lx, int ly);
void mui_list_ensure_visible(struct mui_list *l, int idx);
void mui_list_scroll(struct mui_list *l, int rows);
void mui_list_page(struct mui_list *l, int dir);

void mui_list_paint(struct mui_list *l, struct mui_surface *s, int ax, int ay);

/* 只绘制某一行（可见时）。用于 hover/选中变化的局部刷新。 */
void mui_list_paint_row(struct mui_list *l, struct mui_surface *s,
                        int ax, int ay, int idx);

#endif
