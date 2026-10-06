#include <pty.h>
#include <task.h>
#include <scheduler.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <uaccess.h>
#include <proc.h>
#include <signal.h>

struct pty_pair {
    uint8_t used;
    uint8_t reserved[3];

    uint8_t  to_master[PTY_BUF_SIZE];
    uint32_t tm_head, tm_tail;

    uint8_t  to_slave[PTY_BUF_SIZE];
    uint32_t ts_head, ts_tail;

    char     line_buf[PTY_LINE_MAX];
    int      line_len;

    struct task *wait_master;
    struct task *wait_slave;

    uint32_t fg_pid;    /* HeliumOS：用 pid 而非 pgid */

    int refcount;
    int eof_pending;
};

static struct pty_pair g_pairs[PTY_MAX_PAIRS];

void pty_init(void) {
    memset(g_pairs, 0, sizeof(g_pairs));
}

/* ---------- 环形缓冲 ---------- */

static uint32_t buf_count(uint32_t h, uint32_t t) {
    return (h - t + PTY_BUF_SIZE) % PTY_BUF_SIZE;
}

static int buf_write(uint8_t *b, uint32_t *h, uint32_t t,
                     const uint8_t *data, uint32_t n) {
    uint32_t used = buf_count(*h, t);
    uint32_t space = PTY_BUF_SIZE - 1 - used;
    if (n > space) n = space;
    for (uint32_t i = 0; i < n; i++) {
        b[*h] = data[i];
        *h = (*h + 1) % PTY_BUF_SIZE;
    }
    return (int)n;
}

static int buf_read(uint8_t *b, uint32_t h, uint32_t *t,
                    uint8_t *out, uint32_t n) {
    uint32_t cnt = buf_count(h, *t);
    if (n > cnt) n = cnt;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = b[*t];
        *t = (*t + 1) % PTY_BUF_SIZE;
    }
    return (int)n;
}

/* ---------- 唤醒 ---------- */

static void wake_master(struct pty_pair *p) {
    if (p->wait_master) {
        struct task *t = p->wait_master;
        p->wait_master = NULL;
        unblock_task(t, TASK_STATE_READY);
    }
}

static void wake_slave(struct pty_pair *p) {
    if (p->wait_slave) {
        struct task *t = p->wait_slave;
        p->wait_slave = NULL;
        unblock_task(t, TASK_STATE_READY);
    }
}

/* ---------- 行规程 ---------- */

/* term 写 master 的每个字节过这里。
 * 只做最基本：行缓冲、退格、回车提交、^U 清行。
 * 不做：回显、^C、^D、^Z。 */
static void line_discipline(struct pty_pair *p, uint8_t c) {
    /* ^D（EOF） */
    if (c == 0x04) {
        if (p->line_len > 0) {
            buf_write(p->to_slave, &p->ts_head, p->ts_tail,
                      (uint8_t*)p->line_buf, p->line_len);
            p->line_len = 0;
        } else {
            p->eof_pending = 1;
        }
        wake_slave(p);
        return;
    }

    /* ^C（SIGINT）—— 标准 Unix：清行 + 回显 ^C + 给前台组发信号。
     * HeliumOS：发给 p->fg_pid 单个进程。 */
    if (c == 0x03) {
        p->line_len = 0;

        const char echo[] = "^C\n";
        buf_write(p->to_master, &p->tm_head, p->tm_tail,
                  (uint8_t*)echo, sizeof(echo) - 1);
        wake_master(p);

        if (p->fg_pid != 0) {
            struct task *t = proc_find_by_pid(p->fg_pid);
            if (t && !t->zombie) {
                t->pending_signals |= (1u << SIGINT);

                /* 清本 pair 对它的等待指针，避免 wake 重复入队 */
                if (p->wait_slave == t) p->wait_slave = NULL;
                if (p->wait_master == t) p->wait_master = NULL;

                if (t->state == TASK_STATE_SLEEPING ||
                    t->state == TASK_STATE_WAITING_CHILD ||
                    t->state == TASK_STATE_WAITING_PTY ||
                    t->state == TASK_STATE_WAITING_PIPE) {
                    remove_task_from_queue(&blocked_list_head, &blocked_list_tail, t);
                    t->state = TASK_STATE_READY;
                    enqueue_task(&ready_queue_head, &ready_queue_tail, t);
                }
            }
        }
        return;
    }

    /* ^U（清行） */
    if (c == 0x15) {
        p->line_len = 0;
        return;
    }

    /* 退格 */
    if (c == '\b' || c == 0x7F) {
        if (p->line_len > 0) p->line_len--;
        return;
    }

    /* 回车 */
    if (c == '\r' || c == '\n') {
        if (p->line_len > 0) {
            buf_write(p->to_slave, &p->ts_head, p->ts_tail,
                      (uint8_t*)p->line_buf, p->line_len);
        }
        uint8_t nl = '\n';
        buf_write(p->to_slave, &p->ts_head, p->ts_tail, &nl, 1);
        p->line_len = 0;
        wake_slave(p);
        return;
    }

    /* 普通字符 */
    if (p->line_len < PTY_LINE_MAX - 1) {
        p->line_buf[p->line_len++] = (char)c;
    }
}

/* ---------- 读写 API ---------- */

int pty_write_slave(struct pty_pair *p, const uint8_t *buf, uint32_t n) {
    if (!p || !buf || n == 0) return 0;
    int wrote = buf_write(p->to_master, &p->tm_head, p->tm_tail, buf, n);
    wake_master(p);
    return wrote;
}

int pty_read_slave(struct pty_pair *p, uint8_t *buf, uint32_t n) {
    if (!p || !buf) return EINVAL;
    if (n == 0) return 0;

    while (1) {
        int got = buf_read(p->to_slave, p->ts_head, &p->ts_tail, buf, n);
        if (got > 0) return got;

        if (p->eof_pending) {
            p->eof_pending = 0;
            return 0;   /* EOF */
        }

        struct task *cur = get_current_task();
        if (!cur) return EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals) return EINTR;

        p->wait_slave = cur;
        block_current(TASK_STATE_WAITING_PTY);
        schedule();
        p->wait_slave = NULL;
    }
}

static int pty_write_master(struct pty_pair *p, const uint8_t *buf, uint32_t n) {
    if (!p || !buf || n == 0) return 0;
    for (uint32_t i = 0; i < n; i++) {
        line_discipline(p, buf[i]);
    }
    return (int)n;
}

static int pty_read_master(struct pty_pair *p, uint8_t *buf, uint32_t n) {
    if (!p || !buf) return EINVAL;
    if (n == 0) return 0;

    while (1) {
        int got = buf_read(p->to_master, p->tm_head, &p->tm_tail, buf, n);
        if (got > 0) return got;

        struct task *cur = get_current_task();
        if (!cur) return EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals) return EINTR;

        p->wait_master = cur;
        block_current(TASK_STATE_WAITING_PTY);
        schedule();
        p->wait_master = NULL;
    }
}

/* ---------- 分配/释放 ---------- */

static struct pty_pair *pair_alloc(void) {
    for (int i = 0; i < PTY_MAX_PAIRS; i++) {
        if (!g_pairs[i].used) {
            memset(&g_pairs[i], 0, sizeof(g_pairs[i]));
            g_pairs[i].used = 1;
            return &g_pairs[i];
        }
    }
    return NULL;
}

static int handle_alloc(struct task *t, struct pty_pair *p, int type) {
    for (int i = 0; i < PTY_MAX_HANDLES; i++) {
        if (!t->pty_handles[i].used) {
            t->pty_handles[i].used = 1;
            t->pty_handles[i].type = type;
            t->pty_handles[i].pair = p;
            p->refcount++;
            return i;
        }
    }
    return -1;
}

static void handle_close(struct task *t, int h) {
    if (!t || h < 0 || h >= PTY_MAX_HANDLES) return;
    if (!t->pty_handles[h].used) return;

    struct pty_pair *p = t->pty_handles[h].pair;
    t->pty_handles[h].used = 0;
    t->pty_handles[h].pair = NULL;

    if (p) {
        wake_master(p);
        wake_slave(p);
        if (p->refcount > 0) p->refcount--;
        if (p->refcount == 0) {
            p->used = 0;
        }
    }
}

void pty_release_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < PTY_MAX_HANDLES; i++) {
        if (t->pty_handles[i].used) handle_close(t, i);
    }
}

/* ---------- syscall ---------- */

int sys_pty_open(int *user_fds) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (check_user_range((uint32_t)user_fds, 8) < 0) return EFAULT;

    struct pty_pair *p = pair_alloc();
    if (!p) return ENOSPC;

    int m = handle_alloc(cur, p, PTY_MASTER);
    if (m < 0) { p->used = 0; return ENOSPC; }

    int s = handle_alloc(cur, p, PTY_SLAVE);
    if (s < 0) { handle_close(cur, m); return ENOSPC; }

    int fds[2] = { m, s };
    if (copy_to_user((uint32_t)user_fds, fds, 8) < 0) {
        handle_close(cur, m);
        handle_close(cur, s);
        return EFAULT;
    }
    return OK;
}

int sys_pty_read(int h, void *buf_user, uint32_t n) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;
    if (check_user_range((uint32_t)buf_user, n) < 0) return EFAULT;
    if (n == 0) return 0;

    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;

    uint8_t tmp[512];
    uint32_t chunk = n < sizeof(tmp) ? n : sizeof(tmp);

    int got = (cur->pty_handles[h].type == PTY_MASTER)
              ? pty_read_master(p, tmp, chunk)
              : pty_read_slave(p, tmp, chunk);

    if (got <= 0) return got;
    if (copy_to_user((uint32_t)buf_user, tmp, got) < 0) return EFAULT;
    return got;
}

int sys_pty_write(int h, const void *buf_user, uint32_t n) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;
    if (check_user_range((uint32_t)buf_user, n) < 0) return EFAULT;
    if (n == 0) return 0;

    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;

    uint8_t tmp[512];
    uint32_t chunk = n < sizeof(tmp) ? n : sizeof(tmp);
    if (copy_from_user(tmp, (uint32_t)buf_user, chunk) < 0) return EFAULT;

    return (cur->pty_handles[h].type == PTY_MASTER)
           ? pty_write_master(p, tmp, chunk)
           : pty_write_slave(p, tmp, chunk);
}

int sys_pty_close(int h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;

    handle_close(cur, h);
    return OK;
}

int pty_dup_handle(struct task *parent, int parent_h,
                   struct task *child, int *out_child_h, int *out_type) {
    if (!parent || !child) return -1;
    if (parent_h < 0 || parent_h >= PTY_MAX_HANDLES) return -1;
    if (!parent->pty_handles[parent_h].used) return -1;

    struct pty_pair *p = parent->pty_handles[parent_h].pair;
    int type = parent->pty_handles[parent_h].type;

    int new_h = handle_alloc(child, p, type);
    if (new_h < 0) return -1;

    if (out_child_h) *out_child_h = new_h;
    if (out_type)    *out_type = type;
    return 0;
}

int sys_pty_set_fg(int h, uint32_t pid) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;

    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;

    p->fg_pid = pid;
    return OK;
}