#ifndef PTY_H
#define PTY_H

#include <stdint.h>

#define PTY_MAX_PAIRS   8
#define PTY_MAX_HANDLES 4
#define PTY_BUF_SIZE    4096
#define PTY_LINE_MAX    256

#define PTY_MASTER  1
#define PTY_SLAVE   2

struct pty_pair;
struct task;


struct pty_handle {
    uint8_t used;
    uint8_t type;
    uint16_t reserved;
    struct pty_pair *pair;
};

void pty_init(void);
void pty_release_all(struct task *t);

/* 内核 API（供 io_slot 集成） */
int pty_read_slave(struct pty_pair *p, uint8_t *buf, uint32_t n);
int pty_write_slave(struct pty_pair *p, const uint8_t *buf, uint32_t n);

/* syscall */
int sys_pty_open(int *user_fds);
int sys_pty_read(int h, void *buf_user, uint32_t n);
int sys_pty_write(int h, const void *buf_user, uint32_t n);
int sys_pty_close(int h);
int sys_pty_set_fg(int h, uint32_t pid);
int sys_pty_avail(int h);

/* spawn 用：把 parent 的 handle 复制到 child */
int pty_dup_handle(struct task *parent, int parent_h,
                   struct task *child, int *out_child_h, int *out_type);

#endif