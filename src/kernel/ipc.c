#include <ipc.h>
#include <task.h>
#include <scheduler.h>
#include <vmm.h>
#include <printf.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
#include <uaccess.h>

/* ---------- 全局 pipe 池 ---------- */
static struct pipe g_pipes[IPC_MAX_PIPES];

struct named_entry {
    uint8_t      used;
    char         name[PIPE_NAME_MAX];
    struct pipe *pipe;
};

static struct named_entry g_named[IPC_MAX_NAMED];

void ipc_init(void) {
    memset(g_pipes, 0, sizeof(g_pipes));
    memset(g_named, 0, sizeof(g_named));
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

/* ---------- 名字表 ---------- */

static struct pipe *named_lookup(const char *name) {
    for (int i = 0; i < IPC_MAX_NAMED; i++) {
        if (g_named[i].used && strcmp(g_named[i].name, name) == 0) {
            return g_named[i].pipe;
        }
    }
    return NULL;
}

static int named_register(const char *name, struct pipe *p) {
    for (int i = 0; i < IPC_MAX_NAMED; i++) {
        if (!g_named[i].used) {
            g_named[i].used = 1;
            strncpy(g_named[i].name, name, PIPE_NAME_MAX - 1);
            g_named[i].name[PIPE_NAME_MAX - 1] = '\0';
            g_named[i].pipe = p;
            return 0;
        }
    }
    return -1;
}

/* 返回被摘掉的 pipe；调用方决定是否 free。 */
static struct pipe *named_unregister(const char *name) {
    for (int i = 0; i < IPC_MAX_NAMED; i++) {
        if (g_named[i].used && strcmp(g_named[i].name, name) == 0) {
            struct pipe *p = g_named[i].pipe;
            g_named[i].used = 0;
            g_named[i].pipe = NULL;
            return p;
        }
    }
    return NULL;
}

static int pipe_is_named(struct pipe *p) {
    for (int i = 0; i < IPC_MAX_NAMED; i++) {
        if (g_named[i].used && g_named[i].pipe == p) return 1;
    }
    return 0;
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

        pipe_wakeup_all(p);

        if (p->refcount == 0 && !pipe_is_named(p)) {
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

    uint32_t written = 0;
    while (written < n) {
        uint32_t remaining = n - written;
        uint32_t space = PIPE_BUF_SIZE - p->count;

        /* Keep small messages atomic.  WM/IPC protocols read fixed-size
         * records, so a short write would permanently desynchronise the
         * byte stream and can make a client consume garbage events. */
        if (remaining <= PIPE_BUF_SIZE && space < remaining) {
            /* Wait below until the whole record fits. */
        } else if (space > 0) {
            uint32_t to_write = remaining;
            if (to_write > space) to_write = space;
            for (uint32_t i = 0; i < to_write; i++) {
                p->buf[p->write_pos] = buf[written + i];
                p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            }
            p->count += to_write;
            written += to_write;
            pipe_wakeup_all(p);
            continue;
        }

        if (p->readers == 0) return written ? (int)written : EPIPE;

        struct task *cur = get_current_task();
        if (!cur) return EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals) {
            return written ? (int)written : EINTR;
        }

        cur->wait_pipe = p;
        block_current(TASK_STATE_WAITING_PIPE);
        schedule();
        cur->wait_pipe = NULL;
    }
    return (int)written;
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

/* ---------- 命名 pipe ---------- */

int sys_pipe_named(const char *name_user, int *fds_user) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    char name[PIPE_NAME_MAX];
    if (strncpy_from_user(name, name_user, sizeof(name)) < 0) return EFAULT;
    if (name[0] == '\0') return EINVAL;
    if (check_user_range((uint32_t)fds_user, 8) < 0) return EFAULT;
    if (named_lookup(name)) return EEXIST;

    struct pipe *p = pipe_alloc();
    if (!p) return ENOSPC;

    if (named_register(name, p) < 0) {
        pipe_free(p);
        return ENOSPC;
    }

    int rd = ipc_handle_alloc(cur, p, IPC_PIPE_READ);
    if (rd < 0) {
        named_unregister(name);
        pipe_free(p);
        return ENOSPC;
    }

    int wr = ipc_handle_alloc(cur, p, IPC_PIPE_WRITE);
    if (wr < 0) {
        ipc_handle_close(cur, rd);
        struct pipe *fp = named_unregister(name);
        if (fp && fp->refcount == 0) pipe_free(fp);
        return ENOSPC;
    }

    int fds[2] = { rd, wr };
    if (copy_to_user((uint32_t)fds_user, fds, 8) < 0) {
        ipc_handle_close(cur, rd);
        ipc_handle_close(cur, wr);
        struct pipe *fp = named_unregister(name);
        if (fp && fp->refcount == 0) pipe_free(fp);
        return EFAULT;
    }
    return OK;
}

int sys_pipe_open(const char *name_user, int wr) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    char name[PIPE_NAME_MAX];
    if (strncpy_from_user(name, name_user, sizeof(name)) < 0) return EFAULT;

    struct pipe *p = named_lookup(name);
    if (!p) return ENOENT;

    int type = wr ? IPC_PIPE_WRITE : IPC_PIPE_READ;
    int h = ipc_handle_alloc(cur, p, type);
    if (h < 0) return ENOSPC;
    return h;
}

int sys_pipe_unlink(const char *name_user) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    char name[PIPE_NAME_MAX];
    if (strncpy_from_user(name, name_user, sizeof(name)) < 0) return EFAULT;

    struct pipe *p = named_unregister(name);
    if (!p) return ENOENT;

    if (p->refcount == 0) pipe_free(p);
    return OK;
}

int sys_pipe_avail(int h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= IPC_MAX_HANDLES) return EINVAL;
    if (!cur->ipc_handles[h].used) return EINVAL;

    struct pipe *p = cur->ipc_handles[h].pipe;
    if (!p) return EINVAL;
    return (int)p->count;
}