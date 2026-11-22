#ifndef METHYLUI_SCROLL_H
#define METHYLUI_SCROLL_H

#include "methylui/mui_widget.h"

/* 竖直滚动条控件。
 * 视觉与 mui_list 内建滚动条一致：轨道 + thumb。
 * 交互与文件窗口一致：
 *   - 拖拽 thumb
 *   - 点击 thumb 上方/下方轨道翻页
 *   - 方向键 / PageUp / PageDown / Home / End 滚动（需要获得焦点）
 * 使用者维护 value / total / visible 三个量；value 变化时通过 on_change 通知内容重绘。 */
struct mui_scroll {
    struct mui_widget base;

    int value;              /* 当前首个可见行（0..max_value） */
    int total;              /* 总行数 */
    int visible;            /* 可见行数 */

    unsigned int bar_bg;    /* 轨道背景 */
    unsigned int bar_fg;    /* thumb 常态 */
    unsigned int bar_hover; /* thumb 悬停 */
    unsigned int bar_drag;  /* thumb 拖动中 */

    int dragging;
    int drag_offset;

    /* ESC 序列解析（方向键 / PageUp / PageDown / Home / End） */
    int  esc_state;
    int  esc_len;
    char esc_buf[8];

    /* value 变化时回调，用于让内容区重绘。可为 NULL。 */
    void (*on_change)(struct mui_scroll *s);
};

void mui_scroll_init(struct mui_scroll *s, int x, int y, int w, int h);

/* 设置滚动范围。value 会被夹到 [0, max_value]。 */
void mui_scroll_set_range(struct mui_scroll *s, int total, int visible);

/* 设置当前 value。内部会夹紧并触发 on_change。 */
void mui_scroll_set_value(struct mui_scroll *s, int value);

int  mui_scroll_value(struct mui_scroll *s);
int  mui_scroll_max_value(struct mui_scroll *s);
int  mui_scroll_thumb_h(struct mui_scroll *s);
int  mui_scroll_thumb_y(struct mui_scroll *s, int ay);

void mui_scroll_by(struct mui_scroll *s, int rows);
void mui_scroll_page(struct mui_scroll *s, int dir);
void mui_scroll_home(struct mui_scroll *s);
void mui_scroll_end(struct mui_scroll *s);
void mui_scroll_ensure_visible(struct mui_scroll *s, int idx);

#endif