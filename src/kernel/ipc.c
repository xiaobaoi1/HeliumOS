#include <ipc.h>
#include <task.h>
#include <scheduler.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>

/* ---------- 全局 pipe 池 ---------- */
static struct pipe g_pipes[IPC_MAX_PIPES];

void ipc_init(void) {
    memset(g_pipes, 0, sizeof(g_pipes));
}

/* ---------- pipe 分配/释放 ---------- */
struct pipe *pipe_alloc(void) {
    for (int i = 0; i < IPC_MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            struct pipe *p = &g_pipes[i];
            memset(p, 0, sizeof(*p));
            p->used = 1;
            return p;
        }
    }
    return NULL;
}

void pipe_free(struct pipe *p) {
    if (!p) return;
    p->used = 0;
}

/* ---------- 句柄管理 ---------- */
int ipc_handle_alloc(struct task *t, struct pipe *p, int type) {
    if (!t || !p) return -1;
    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        if (!t->ipc_handles[i].used) {
            t->ipc_handles[i].used = 1;
            t->ipc_handles[i].type = type;
            t->ipc_handles[i].pipe = p;
            if (type == IPC_PIPE_READ) p->readers++;
            else                       p->writers++;
            p->refcount++;
            return i;
        }
    }
    return -1;
}

void ipc_handle_close(struct task *t, int h) {
    if (!t || h < 0 || h >= IPC_MAX_HANDLES) return;
    if (!t->ipc_handles[h].used) return;

    struct pipe *p = t->ipc_handles[h].pipe;
    int type = t->ipc_handles[h].type;

    t->ipc_handles[h].used = 0;
    t->ipc_handles[h].pipe = NULL;

    if (p) {
        if (type == IPC_PIPE_READ) p->readers--;
        else                       p->writers--;
        p->refcount--;

        /* 关闭后唤醒所有等这个 pipe 的进程 */
        pipe_wakeup_all(p);

        if (p->refcount == 0) {
            pipe_free(p);
        }
    }
}

/* ---------- 唤醒 ---------- */
void pipe_wakeup_all(struct pipe *p) {
    if (!p) return;
    for (struct task *t = blocked_list_head; t; ) {
        struct task *next = t->next;
        if (t->state == TASK_STATE_WAITING_PIPE && t->wait_pipe == p) {
            t->wait_pipe = NULL;
            unblock_task(t, TASK_STATE_READY);
        }
        t = next;
    }
}

/* ---------- 读 ---------- */
int pipe_read(struct pipe *p, uint8_t *buf, uint32_t n) {
    if (!p || !buf) return EINVAL;
    if (n == 0) return 0;

    while (1) {
        /* 有数据 */
        if (p->count > 0) {
            uint32_t to_read = n < p->count ? n : p->count;
            for (uint32_t i = 0; i < to_read; i++) {
                buf[i] = p->buf[p->read_pos];
                p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count -= to_read;

            /* 读完了——唤醒等写的人 */
            pipe_wakeup_all(p);
            return (int)to_read;
        }

        /* 无数据且没有写端 → EOF */
        if (p->writers == 0) {
            return 0;
        }

        /* 阻塞前检查信号 */
        struct task *cur = get_current_task();
        if (!cur) return EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals) {
            return EINTR;
        }

        cur->wait_pipe = p;
        block_current(TASK_STATE_WAITING_PIPE);
        schedule();
        cur->wait_pipe = NULL;
    }
}

/* ---------- 写 ---------- */
int pipe_write(struct pipe *p, const uint8_t *buf, uint32_t n) {
    if (!p || !buf) return EINVAL;
    if (n == 0) return 0;

    while (1) {
        /* 有空间 */
        if (p->count < PIPE_BUF_SIZE) {
            uint32_t space = PIPE_BUF_SIZE - p->count;
            uint32_t to_write = n < space ? n : space;
            for (uint32_t i = 0; i < to_write; i++) {
                p->buf[p->write_pos] = buf[i];
                p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count += to_write;

            /* 写完——唤醒等读的人 */
            pipe_wakeup_all(p);
            return (int)to_write;
        }

        /* 无空间且没有读端 → EPIPE */
        if (p->readers == 0) {
            return EPIPE;
        }

        /* 阻塞前检查信号 */
        struct task *cur = get_current_task();
        if (!cur) return EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals) {
            return EINTR;
        }

        cur->wait_pipe = p;
        block_current(TASK_STATE_WAITING_PIPE);
        schedule();
        cur->wait_pipe = NULL;
    }
}

/* ---------- spawn 用：复制句柄 ---------- */
int ipc_dup_handle(struct task *parent, int parent_h, struct task *child,
                   int *out_child_h, int *out_child_type) {
    if (!parent || !child) return -1;
    if (parent_h < 0 || parent_h >= IPC_MAX_HANDLES) return -1;
    if (!parent->ipc_handles[parent_h].used) return -1;

    struct pipe *p = parent->ipc_handles[parent_h].pipe;
    int type = parent->ipc_handles[parent_h].type;

    int new_h = ipc_handle_alloc(child, p, type);
    if (new_h < 0) return -1;

    if (out_child_h) *out_child_h = new_h;
    if (out_child_type) *out_child_type = type;
    return 0;
}

/* ---------- 进程退出清理 ---------- */
void ipc_release_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < IPC_MAX_HANDLES; i++) {
        if (t->ipc_handles[i].used) {
            ipc_handle_close(t, i);
        }
    }
}

/* ---------- 系统调用 ---------- */
int sys_pipe(int *fds_user) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    if (check_user_range((uint32_t)fds_user, 8) < 0) return EFAULT;

    struct pipe *p = pipe_alloc();
    if (!p) return ENOSPC;

    int rd = ipc_handle_alloc(cur, p, IPC_PIPE_READ);
    if (rd < 0) { pipe_free(p); return ENOSPC; }

    int wr = ipc_handle_alloc(cur, p, IPC_PIPE_WRITE);
    if (wr < 0) {
        ipc_handle_close(cur, rd);
        return ENOSPC;
    }

    fds_user[0] = rd;
    fds_user[1] = wr;
    return OK;
}

int sys_pipe_read(int h, void *buf_user, uint32_t n) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
    if (!cur->ipc_handles[h].used) return EINVAL;
    if (cur->ipc_handles[h].type != IPC_PIPE_READ) return EINVAL;
    if (check_user_range((uint32_t)buf_user, n) < 0) return EFAULT;
    if (n == 0) return 0;

    return pipe_read(cur->ipc_handles[h].pipe, (uint8_t*)buf_user, n);
}

int sys_pipe_write(int h, const void *buf_user, uint32_t n) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
    if (!cur->ipc_handles[h].used) return EINVAL;
    if (cur->ipc_handles[h].type != IPC_PIPE_WRITE) return EINVAL;
    if (check_user_range((uint32_t)buf_user, n) < 0) return EFAULT;
    if (n == 0) return 0;

    return pipe_write(cur->ipc_handles[h].pipe, (const uint8_t*)buf_user, n);
}

int sys_ipc_close(int h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
    if (!cur->ipc_handles[h].used) return EINVAL;

    ipc_handle_close(cur, h);
    return OK;
}