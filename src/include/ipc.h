#ifndef IPC_H
#define IPC_H

#include <stdint.h>

/* ---------- 常量 ---------- */
#define IPC_MAX_PIPES     16
#define IPC_MAX_HANDLES   8
#define PIPE_BUF_SIZE     4096

/* ---------- handle 类型 ---------- */
#define IPC_PIPE_READ     1
#define IPC_PIPE_WRITE    2

/* ---------- pipe ---------- */
struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    uint32_t refcount;
    uint8_t  readers;
    uint8_t  writers;
    uint8_t  used;
    uint8_t  reserved;
};

/* ---------- 每进程句柄 ---------- */
struct ipc_handle {
    uint8_t       used;
    uint8_t       type;
    uint16_t      reserved;
    struct pipe  *pipe;
};

/* 前向声明 */
struct task;

/* ---------- 生命周期 ---------- */
void  ipc_init(void);
void  ipc_release_all(struct task *t);

/* ---------- pipe 对象 ---------- */
struct pipe *pipe_alloc(void);
void         pipe_free(struct pipe *p);

/* ---------- 句柄分配 ---------- */
int   ipc_handle_alloc(struct task *t, struct pipe *p, int type);
void  ipc_handle_close(struct task *t, int h);

/* ---------- 读写 ---------- */
int   pipe_read(struct pipe *p, uint8_t *buf, uint32_t n);
int   pipe_write(struct pipe *p, const uint8_t *buf, uint32_t n);

/* ---------- spawn 用：复制父进程的 handle 到子进程 ---------- */
int   ipc_dup_handle(struct task *parent, int parent_h, struct task *child,
                     int *out_child_h, int *out_child_type);

/* ---------- 唤醒 ---------- */
void  pipe_wakeup_all(struct pipe *p);

/* ---------- 系统调用 ---------- */
int   sys_pipe(int *fds_user);
int   sys_pipe_read(int h, void *buf_user, uint32_t n);
int   sys_pipe_write(int h, const void *buf_user, uint32_t n);
int   sys_ipc_close(int h);

#endif