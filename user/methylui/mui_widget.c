#include "methylui/mui_widget.h"
#include <stddef.h>

void mui_widget_init(struct mui_widget *widget, int x, int y, int width, int height) {
    if (!widget) return;
    widget->x = x;
    widget->y = y;
    widget->w = width;
    widget->h = height;
    widget->visible = 1;
    widget->dirty = 1;
    widget->hover = 0;
    widget->pressed = 0;
    widget->focused = 0;
    widget->disabled = 0;
    widget->focusable = 0;
    widget->parent = NULL;
    widget->first_child = NULL;
    widget->last_child = NULL;
    widget->prev_sibling = NULL;
    widget->next_sibling = NULL;
    widget->user = NULL;
    widget->on_paint = NULL;
    widget->on_mouse_down = NULL;
    widget->on_mouse_up = NULL;
    widget->on_mouse_move = NULL;
    widget->on_mouse_enter = NULL;
    widget->on_mouse_leave = NULL;
    widget->on_focus_in = NULL;
    widget->on_focus_out = NULL;
    widget->on_key = NULL;
}

void mui_widget_add_child(struct mui_widget *parent, struct mui_widget *child) {
    if (!parent || !child) return;
    child->parent = parent;
    child->prev_sibling = parent->last_child;
    child->next_sibling = NULL;
    if (parent->last_child) parent->last_child->next_sibling = child;
    else                    parent->first_child = child;
    parent->last_child = child;
    parent->dirty = 1;
}

void mui_widget_remove_child(struct mui_widget *parent, struct mui_widget *child) {
    if (!parent || !child || child->parent != parent) return;

    if (child->prev_sibling)
        child->prev_sibling->next_sibling = child->next_sibling;
    else
        parent->first_child = child->next_sibling;

    if (child->next_sibling)
        child->next_sibling->prev_sibling = child->prev_sibling;
    else
        parent->last_child = child->prev_sibling;

    child->parent = NULL;
    child->prev_sibling = NULL;
    child->next_sibling = NULL;
    parent->dirty = 1;
}

void mui_widget_invalidate(struct mui_widget *w) {
    if (!w) return;
    w->dirty = 1;
    for (struct mui_widget *c = w->first_child; c; c = c->next_sibling)
        mui_widget_invalidate(c);
}

int mui_widget_abs_x(struct mui_widget *w) {
    int x = 0;
    for (; w; w = w->parent) x += w->x;
    return x;
}

int mui_widget_abs_y(struct mui_widget *w) {
    int y = 0;
    for (; w; w = w->parent) y += w->y;
    return y;
}

void mui_widget_paint(struct mui_widget *w, struct mui_surface *s,
                      int ax, int ay) {
    if (!w || !w->visible) return;

    int children_need_paint = 0;

    if (w->dirty) {
        if (w->on_paint) w->on_paint(w, s, ax, ay);
        w->dirty = 0;
        children_need_paint = 1;
    }

    for (struct mui_widget *c = w->first_child; c; c = c->next_sibling) {
        if (children_need_paint) c->dirty = 1;
        mui_widget_paint(c, s, ax + c->x, ay + c->y);
    }
}

struct mui_widget *mui_widget_hit_test(struct mui_widget *root, int x, int y) {
    if (!root || !root->visible) return NULL;
    if (x < 0 || y < 0 || x >= root->w || y >= root->h) return NULL;

    /* 从后往前——后添加的在上层 */
    for (struct mui_widget *c = root->last_child; c; c = c->prev_sibling) {
        struct mui_widget *hit = mui_widget_hit_test(c, x - c->x, y - c->y);
        if (hit) return hit;
    }

    return root;
}