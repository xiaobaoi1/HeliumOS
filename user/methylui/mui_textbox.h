#ifndef METHYLUI_TEXTBOX_H
#define METHYLUI_TEXTBOX_H

#include "methylui/mui_widget.h"

#define MUI_TEXTBOX_MAX_LEN 128

struct mui_textbox {
    struct mui_widget base;
    char text[MUI_TEXTBOX_MAX_LEN + 1];
    int  len;
    int  cursor;
    int  scroll;              /* 水平滚动偏移（字符数） */

    unsigned int bg;
    unsigned int fg;
    unsigned int border;
    unsigned int focus_border;
};

void mui_textbox_init(struct mui_textbox *t, int x, int y, int w);

const char *mui_textbox_get_text(struct mui_textbox *t);
void mui_textbox_set_text(struct mui_textbox *t, const char *text);
void mui_textbox_insert_utf8(struct mui_textbox *t, const char *text);

/* 光标移动：delta 为字符数；home/end 移到首/尾。 */
void mui_textbox_move_cursor(struct mui_textbox *t, int delta);
void mui_textbox_cursor_home(struct mui_textbox *t);
void mui_textbox_cursor_end(struct mui_textbox *t);

#endif