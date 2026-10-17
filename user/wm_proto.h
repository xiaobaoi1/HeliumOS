#ifndef USER_WM_PROTO_H
#define USER_WM_PROTO_H

#include <stdint.h>

#define WM_MAGIC       0x574D5231   /* "WMR1" */
#define WM_INBOX_NAME  "wm-inbox"

#define WM_REQ_CREATE  1
#define WM_REQ_DESTROY 2

/* 应用 → WM。写端 pipe_open("wm-inbox", 1)。
 * 每次写 sizeof(struct wm_request) 字节。
 * 应用自己负责：shm 创建/填充、ev pipe 创建（保留读端）、counter 更新。 */
struct wm_request {
    uint32_t magic;
    uint32_t type;
    uint32_t w, h;             /* 客户区尺寸（不含边框/标题栏） */
    uint32_t owner_pid;
    char     shm_name[16];     /* 应用创建，格式 "8B counter + w*h*4 pixels" */
    char     ev_name[16];      /* 应用创建命名 pipe，应用保留读端 */
    char     title[32];
};

#endif