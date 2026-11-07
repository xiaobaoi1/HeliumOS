/* Methylate.c - MUI 库功能演示程序 */

#include "methylui/mui.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

/* ============================================================
 * 自定义按钮控件
 * ============================================================ */

struct my_button {
    struct mui_widget base;
    char label[64];
    unsigned int bg;
    unsigned int hover_bg;
    unsigned int pressed_bg;
    unsigned int fg;
    unsigned int border;
    void (*on_click)(struct my_button *b);
};

static void button_paint(struct mui_widget *w, struct mui_surface *s,
                         int ax, int ay) {
    struct my_button *b = (struct my_button *)w;
    unsigned int bg = b->bg;
    if (w->pressed)      bg = b->pressed_bg;
    else if (w->hover)   bg = b->hover_bg;

    mui_fill(s, ax, ay, w->w, w->h, bg);
    mui_border(s, ax, ay, w->w, w->h, b->border);

    int tw = mui_text_width(b->label);
    int tx = ax + (w->w - tw) / 2;
    int ty = ay + (w->h - MUI_FONT_H) / 2;
    mui_text(s, tx, ty, b->label, b->fg, bg);
}

static void button_mouse_up(struct mui_widget *w, int x, int y, int btn) {
    (void)x; (void)y; (void)btn;
    struct my_button *b = (struct my_button *)w;
    if (b->on_click) b->on_click(b);
}

static void button_init(struct my_button *b, int x, int y, int w, int h,
                        const char *label) {
    memset(b, 0, sizeof(*b));
    mui_widget_init(&b->base, x, y, w, h);
    b->base.on_paint     = button_paint;
    b->base.on_mouse_up  = button_mouse_up;
    b->base.focusable    = 1;
    strncpy(b->label, label, sizeof(b->label) - 1);
    b->bg         = 0x3A6EA5;
    b->hover_bg   = 0x4A7EB5;
    b->pressed_bg = 0x2A5E95;
    b->fg         = 0xFFFFFF;
    b->border     = 0x1A2A3E;
}

/* ============================================================
 * 全局状态
 * ============================================================ */

static struct mui_context  g_ctx;
static struct mui_widget   g_root;
static struct mui_textbox  g_textbox;
static struct mui_label    g_result;
static struct mui_label    g_status;

static mui_key_fn          g_orig_textbox_key;

/* ============================================================
 * mui_scroll 演示：40 行日志 + 竖直滚动条
 * ============================================================ */

#define SCROLL_DEMO_LINES 40
#define SCROLL_DEMO_ROW_H 16

static struct mui_scroll g_scroll;
static struct mui_widget g_scroll_content;

static void scroll_content_paint(struct mui_widget *w, struct mui_surface *s,
                                 int ax, int ay) {
    mui_fill(s, ax, ay, w->w, w->h, 0x0E0E1A);
    mui_border(s, ax, ay, w->w, w->h, 0x44446A);

    int visible = w->h / SCROLL_DEMO_ROW_H;
    char line[48];
    for (int i = 0; i < visible; i++) {
        int idx = g_scroll.value + i;
        if (idx >= g_scroll.total) break;
        snprintf(line, sizeof(line), "Line %02d: hello MUI", idx + 1);
        mui_text(s, ax + 6,
                 ay + i * SCROLL_DEMO_ROW_H + (SCROLL_DEMO_ROW_H - MUI_FONT_H) / 2,
                 line, 0xCCCCCC, 0x0E0E1A);
    }
}

static void scroll_on_change(struct mui_scroll *s) {
    (void)s;
    g_scroll_content.dirty = 1;
}

/* 让内容区也能触发滚动条的键盘处理：焦点在内容区时把按键转发给滚动条 */
static void scroll_content_key(struct mui_widget *w, int key) {
    (void)w;
    if (g_scroll.base.on_key)
        g_scroll.base.on_key(&g_scroll.base, key);
}

/* ============================================================
 * 焦点辅助（复制自 mui_context.c 内部逻辑）
 * ============================================================ */

static void focus_widget(struct mui_widget *w) {
    if (g_ctx.focus == w) return;
    if (g_ctx.focus) {
        g_ctx.focus->focused = 0;
        g_ctx.focus->dirty   = 1;
        if (g_ctx.focus->on_focus_out) g_ctx.focus->on_focus_out(g_ctx.focus);
    }
    g_ctx.focus = w;
    if (w) {
        w->focused = 1;
        w->dirty   = 1;
        if (w->on_focus_in) w->on_focus_in(w);
    }
}

/* ============================================================
 * 事件处理
 * ============================================================ */

static void textbox_key_wrapper(struct mui_widget *w, int key) {
    if (g_orig_textbox_key) g_orig_textbox_key(w, key);
    struct mui_textbox *t = (struct mui_textbox *)w;
    char buf[128];
    snprintf(buf, sizeof(buf), "%-40s", t->text);
    mui_label_set_text(&g_result, buf);
}

static void on_update_click(struct my_button *b) {
    (void)b;
    const char *txt = mui_textbox_get_text(&g_textbox);
    char buf[128];
    snprintf(buf, sizeof(buf), "%-40s", txt[0] ? txt : "(empty)");
    mui_label_set_text(&g_result, buf);
}

static void on_clear_click(struct my_button *b) {
    (void)b;
    mui_textbox_set_text(&g_textbox, "");
    mui_label_set_text(&g_result, "");
    focus_widget(&g_textbox.base);
}

static void on_quit_click(struct my_button *b) {
    (void)b;
    g_ctx.close_requested = 1;
}

static void root_mouse_move(struct mui_widget *w, int x, int y) {
    (void)w;
    char buf[48];
    snprintf(buf, sizeof(buf), "Mouse: %3d,%3d      ", x, y);
    mui_label_set_text(&g_status, buf);
}

/* ============================================================
 * 主程序
 * ============================================================ */

int main(void) {
    if (mui_context_init(&g_ctx, "Methylate - MUI Demo",
                         "methylate_shm", "methylate_ev",
                         480, 360, 240, 180, 0, 0) < 0) {
        const char *msg = "mui_context_init failed\n";
        write(2, msg, strlen(msg));
        return 1;
    }

    /* 根容器 —— 覆盖整个窗口，提供鼠标坐标跟踪 */
    mui_widget_init(&g_root, 0, 0, 480, 360);
    g_root.on_mouse_move = root_mouse_move;
    g_ctx.root = &g_root;

    /* 标题 */
    static struct mui_label title;
    mui_label_init(&title, 20, 20, "Methylate - MUI Demo", 0xFFFFFF, 0x1A1A2E);
    mui_widget_add_child(&g_root, &title.base);

    /* Input 提示标签 */
    static struct mui_label lbl_input;
    mui_label_init(&lbl_input, 20, 62, "Input:", 0xCCCCCC, 0x1A1A2E);
    mui_widget_add_child(&g_root, &lbl_input.base);

    /* 文本框 */
    mui_textbox_init(&g_textbox, 100, 60, 200);
    mui_textbox_set_text(&g_textbox, "Hello, MUI!");
    g_orig_textbox_key    = g_textbox.base.on_key;
    g_textbox.base.on_key = textbox_key_wrapper;
    mui_widget_add_child(&g_root, &g_textbox.base);

    /* Update 按钮 */
    static struct my_button btn_update;
    button_init(&btn_update, 310, 60, 80, 24, "Update");
    btn_update.on_click = on_update_click;
    mui_widget_add_child(&g_root, &btn_update.base);

    /* Clear 按钮 */
    static struct my_button btn_clear;
    button_init(&btn_clear, 400, 60, 60, 24, "Clear");
    btn_clear.on_click = on_clear_click;
    mui_widget_add_child(&g_root, &btn_clear.base);

    /* Result 提示标签 */
    static struct mui_label lbl_result;
    mui_label_init(&lbl_result, 20, 102, "Result:", 0xCCCCCC, 0x1A1A2E);
    mui_widget_add_child(&g_root, &lbl_result.base);

    /* Result 值标签 */
    mui_label_init(&g_result, 100, 102, "(nothing)", 0x00FF88, 0x1A1A2E);
    mui_widget_add_child(&g_root, &g_result.base);

    /* ---------- mui_scroll 演示：40 行日志 + 竖直滚动条 ---------- */
    {
        int sx = 20, sy = 140, sh = 160;
        int content_w = 212;

        mui_widget_init(&g_scroll_content, sx, sy, content_w, sh);
        g_scroll_content.on_paint  = scroll_content_paint;
        g_scroll_content.on_key    = scroll_content_key;
        g_scroll_content.focusable = 1;
        mui_widget_add_child(&g_root, &g_scroll_content);

        mui_scroll_init(&g_scroll, sx + content_w, sy, 8, sh);
        g_scroll.on_change = scroll_on_change;
        mui_scroll_set_range(&g_scroll, SCROLL_DEMO_LINES, sh / SCROLL_DEMO_ROW_H);
        mui_widget_add_child(&g_root, &g_scroll.base);
    }

    /* Quit 按钮 */
    static struct my_button btn_quit;
    button_init(&btn_quit, 400, 200, 60, 24, "Quit");
    btn_quit.bg         = 0xAA4444;
    btn_quit.hover_bg   = 0xBB5555;
    btn_quit.pressed_bg = 0x883333;
    btn_quit.on_click   = on_quit_click;
    mui_widget_add_child(&g_root, &btn_quit.base);

    /* 鼠标坐标状态栏 */
    mui_label_init(&g_status, 20, 330,
                   "Mouse: ---,---      ", 0x8888AA, 0x1A1A2E);
    mui_widget_add_child(&g_root, &g_status.base);

    /* 运行事件循环 */
    mui_context_run(&g_ctx);

    /* 清理 */
    mui_context_destroy(&g_ctx);
    return 0;
}