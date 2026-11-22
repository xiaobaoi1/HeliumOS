#ifndef METHYLUI_WIDGET_H
#define METHYLUI_WIDGET_H

#include "methylui/mui_surface.h"

struct mui_widget;

typedef void (*mui_paint_fn)(struct mui_widget *w, struct mui_surface *s,
                             int ax, int ay);
typedef void (*mui_mouse_fn)(struct mui_widget *w, int x, int y, int btn);
typedef void (*mui_mouse_move_fn)(struct mui_widget *w, int x, int y);
typedef void (*mui_widget_fn)(struct mui_widget *w);
typedef void (*mui_key_fn)(struct mui_widget *w, int key);

struct mui_widget {
    int x, y, w, h;              /* 相对父容器 */
    int visible;
    int dirty;

    int hover;
    int pressed;
    int focused;
    int disabled;
    int focusable;

    struct mui_widget *parent;
    struct mui_widget *first_child;
    struct mui_widget *last_child;
    struct mui_widget *prev_sibling;
    struct mui_widget *next_sibling;

    void *user;

    mui_paint_fn      on_paint;
    mui_mouse_fn      on_mouse_down;
    mui_mouse_fn      on_mouse_up;
    mui_mouse_move_fn on_mouse_move;
    mui_widget_fn     on_mouse_enter;
    mui_widget_fn     on_mouse_leave;
    mui_widget_fn     on_focus_in;
    mui_widget_fn     on_focus_out;
    mui_key_fn        on_key;
};

void mui_widget_init(struct mui_widget *widget, int x, int y, int width, int height);
void mui_widget_add_child(struct mui_widget *parent, struct mui_widget *child);
void mui_widget_remove_child(struct mui_widget *parent, struct mui_widget *child);

void mui_widget_invalidate(struct mui_widget *w);

int mui_widget_abs_x(struct mui_widget *w);
int mui_widget_abs_y(struct mui_widget *w);

void mui_widget_paint(struct mui_widget *w, struct mui_surface *s,
                      int ax, int ay);

struct mui_widget *mui_widget_hit_test(struct mui_widget *root, int x, int y);

#endif