#ifndef USER_WM_PROTO_H
#define USER_WM_PROTO_H

#include <stdint.h>

#define WM_MAGIC       0x574D5231
#define WM_INBOX_NAME  "wm-inbox"

/* 请求类型 */
#define WM_REQ_CREATE       1
#define WM_REQ_DESTROY      2
#define WM_REQ_RESIZE       3
#define WM_REQ_WIN_ACTION   4
#define WM_REQ_SET_CURSOR   5

/* 窗口 kind */
#define WM_KIND_NORMAL      0
#define WM_KIND_DOCK        1
#define WM_KIND_POPUP       2
#define WM_KIND_WALLPAPER   3

/* 事件类型（WM → client） */
#define WM_EV_KEY           1   /* code = ASCII */
#define WM_EV_MOUSE_DOWN    2   /* code = WM_BTN_*, x/y 客户区相对坐标 */
#define WM_EV_MOUSE_UP      3
#define WM_EV_MOUSE_MOVE    4   /* code = 当前按钮掩码 */
#define WM_EV_MOUSE_ENTER   5
#define WM_EV_MOUSE_LEAVE   6
#define WM_EV_FOCUS_IN      7
#define WM_EV_FOCUS_OUT     8
#define WM_EV_RESIZE        9   /* code = new_w, extra = new_h */
#define WM_EV_CLOSE_REQUEST 10
#define WM_EV_CLOSE         11  /* WM 主动销毁时通知（POPUP） */
#define WM_EV_WINDOW_LIST   12  /* 后接 n 个 wm_window_entry */

#define WM_BTN_LEFT         1
#define WM_BTN_RIGHT        2
#define WM_BTN_MIDDLE       4

struct wm_event {
    uint32_t type;
    uint32_t code;
    int32_t  x, y;
    uint32_t extra;
};

/* 光标形状 */
#define WM_CURSOR_ARROW     0
#define WM_CURSOR_HAND      1
#define WM_CURSOR_TEXT      2
#define WM_CURSOR_RESIZE_H  3
#define WM_CURSOR_RESIZE_V  4
#define WM_CURSOR_MOVE      5

/* 窗口动作 */
#define WM_ACTION_MINIMIZE  1
#define WM_ACTION_RESTORE   2
#define WM_ACTION_RAISE     3
#define WM_ACTION_MAXIMIZE  4

/* 窗口列表项（接在 WM_EV_WINDOW_LIST 事件之后） */
struct wm_window_entry {
    uint32_t id;
    uint32_t minimized;
    char     title[32];
};

/* 创建请求 */
struct wm_request {
    uint32_t magic;
    uint32_t type;
    uint32_t kind;
    uint32_t x, y;
    uint32_t w, h;
    uint32_t min_w, min_h;   /* 0 = 无约束 */
    uint32_t max_w, max_h;   /* 0 = 无约束 */
    uint32_t owner_pid;
    char     shm_name[16];
    char     ev_name[16];
    char     title[32];
};

struct wm_destroy_request {
    uint32_t magic;
    uint32_t type;
    uint32_t owner_pid;
    char     shm_name[16];
};

struct wm_resize_request {
    uint32_t magic;
    uint32_t type;
    uint32_t owner_pid;
    char     old_shm[16];
    char     new_shm[16];
    uint32_t new_w, new_h;
};

struct wm_action_request {
    uint32_t magic;
    uint32_t type;
    uint32_t owner_pid;
    uint32_t win_id;
    uint32_t action;
};

struct wm_set_cursor_request {
    uint32_t magic;
    uint32_t type;
    uint32_t owner_pid;
    uint32_t cursor;
};

#endif