#ifndef METHYLUI_LABEL_H
#define METHYLUI_LABEL_H

#include "methylui/mui_widget.h"

struct mui_label {
    struct mui_widget base;
    char text[128];
    unsigned int fg;
    unsigned int bg;
};

void mui_label_init(struct mui_label *l, int x, int y,
                    const char *text, unsigned int fg, unsigned int bg);

void mui_label_set_text(struct mui_label *l, const char *text);

#endif