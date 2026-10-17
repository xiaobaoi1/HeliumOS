#ifndef USER_WM_PROTO_H
#define USER_WM_PROTO_H

#include <stdint.h>

#define WM_MAGIC       0x574D5231
#define WM_INBOX_NAME  "wm-inbox"

/* 请求类型 */
#define WM_REQ_CREATE      1
#define WM_REQ_DESTROY     2
#define WM_REQ_RESIZE      3
#define WM_REQ_WIN_ACTION  4

/* 窗口 kind */
#define WM_KIND_NORMAL     0
#define WM_KIND_DOCK       1
#define WM_KIND_POPUP      2
#define WM_KIND_WALLPAPER  3

/* 事件类型 */
#define WM_EV_KEY          1
#define WM_EV_MOUSE_MOVE   2
#define WM_EV_MOUSE_BTN    3
#define WM_EV_CLOSE        4
#define WM_EV_WINDOW_LIST  5

/* 窗口动作 */
#define WM_ACTION_MINIMIZE 1
#define WM_ACTION_RESTORE  2
#define WM_ACTION_RAISE    3

struct wm_event {
    uint32_t type;
    uint32_t code;
    int32_t  x, y;
};

struct wm_window_entry {
    uint32_t id;
    uint32_t minimized;
    char     title[32];
};

struct wm_request {
    uint32_t magic;
    uint32_t type;
    uint32_t kind;
    uint32_t x, y;
    uint32_t w, h;
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
    uint32_t new_w;
    uint32_t new_h;
};

struct wm_action_request {
    uint32_t magic;
    uint32_t type;
    uint32_t owner_pid;
    uint32_t win_id;
    uint32_t action;
};

#endif