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
    uint8_t esc_state;
    uint8_t reserved[2];

    uint8_t  to_master[PTY_BUF_SIZE];
    uint32_t tm_head, tm_tail;

    uint8_t  to_slave[PTY_BUF_SIZE];
    uint32_t ts_head, ts_tail;

    char     line_buf[PTY_LINE_MAX];
    int      line_len;

    uint16_t cols;
    uint16_t rows;
    uint8_t  raw;

    struct task *wait_master;
    struct task *wait_slave;
    struct task *wait_slave_write;

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
    /* 顺带唤醒等写 master 的 slave */
    if (p->wait_slave_write) {
        uint32_t used = buf_count(p->tm_head, p->tm_tail);
        if (used < PTY_BUF_SIZE - 1) {
            struct task *t = p->wait_slave_write;
            p->wait_slave_write = NULL;
            unblock_task(t, TASK_STATE_READY);
        }
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
    /* ESC 序列过滤：cooked mode 下丢弃 \x1b[... 序列。
     * 方向键、Delete、功能键在 shell 里不该有作用。 */
    if (p->esc_state == 1) {
        if (c == '[') { p->esc_state = 2; return; }
        p->esc_state = 0;
        return;
    }
    if (p->esc_state == 2) {
        if ((c >= '0' && c <= '9') || c == ';' || c == '?') return;
        p->esc_state = 0;
        return;
    }
    if (c == 0x1B) { p->esc_state = 1; return; }

    /* 其他控制字符过滤（保留 \n \r \b \t ^C ^D ^U）。
     * Ctrl+S/Q/F 等在 shell 里不该显示或存入行缓冲。 */
    if (c < 0x20 && c != '\n' && c != '\r' && c != '\b' && c != '\t' &&
        c != 0x03 && c != 0x04 && c != 0x15) {
        return;
    }
    
    /* 小工具：往 to_master 写数据 + 唤醒 */
    #define ECHO(s, n) do { \
        buf_write(p->to_master, &p->tm_head, p->tm_tail, \
                  (const uint8_t*)(s), (n)); \
        wake_master(p); \
    } while (0)

    /* ^D（EOF）—— 不回显 */
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

    /* ^C（SIGINT）—— 清行 + 回显 ^C\n */
    if (c == 0x03) {
        p->line_len = 0;
        ECHO("^C\n", 3);

        if (p->fg_pid != 0) {
            struct task *t = proc_find_by_pid(p->fg_pid);
            if (t && !t->zombie) {
                t->pending_signals |= (1u << SIGINT);
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

    /* ^U（清行）—— 回显 \r + N 空格 + \r */
    if (c == 0x15) {
        int n = p->line_len;
        p->line_len = 0;
        ECHO("\r", 1);
        for (int i = 0; i < n; i++) ECHO(" ", 1);
        ECHO("\r", 1);
        return;
    }

    /* 退格 —— 回显 \b \b */
    if (c == '\b' || c == 0x7F) {
        if (p->line_len > 0) {
            p->line_len--;
            ECHO("\b \b", 3);
        }
        return;
    }

    /* 回车 —— 提交给 slave + 回显 \n */
    if (c == '\r' || c == '\n') {
        if (p->line_len > 0) {
            buf_write(p->to_slave, &p->ts_head, p->ts_tail,
                      (uint8_t*)p->line_buf, p->line_len);
        }
        uint8_t nl = '\n';
        buf_write(p->to_slave, &p->ts_head, p->ts_tail, &nl, 1);
        p->line_len = 0;
        wake_slave(p);

        ECHO("\n", 1);
        return;
    }

    /* 普通字符 —— 存 + 回显 */
    if (p->line_len < PTY_LINE_MAX - 1) {
        p->line_buf[p->line_len++] = (char)c;
        ECHO(&c, 1);
    }

    #undef ECHO
}

/* ---------- 读写 API ---------- */

int pty_write_slave(struct pty_pair *p, const uint8_t *buf, uint32_t n) {
    if (!p || !buf || n == 0) return 0;

    uint32_t written = 0;
    while (written < n) {
        int w = buf_write(p->to_master, &p->tm_head, p->tm_tail,
                          buf + written, n - written);
        if (w > 0) {
            written += w;
            wake_master(p);
            if (written == n) break;
        }

        /* 缓冲满——阻塞等 master 读走 */
        struct task *cur = get_current_task();
        if (!cur) return written > 0 ? (int)written : EINVAL;
        if (cur->pending_signals & ~cur->blocked_signals)
            return written > 0 ? (int)written : EINTR;

        p->wait_slave_write = cur;
        block_current(TASK_STATE_WAITING_PTY);
        schedule();
        p->wait_slave_write = NULL;
    }
    return (int)written;
}

int pty_read_slave(struct pty_pair *p, uint8_t *buf, uint32_t n) {
    if (!p || !buf) return EINVAL;
    if (n == 0) return 0;

    while (1) {
        int got = buf_read(p->to_slave, p->ts_head, &p->ts_tail, buf, n);
        if (got > 0) return got; /* 唤醒等写 to_slave 的 master（暂时没有，占位） */

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

    if (p->raw) {
        for (uint32_t i = 0; i < n; i++) {
            if (buf_count(p->ts_head, p->ts_tail) >= PTY_BUF_SIZE - 1)
                break;
            p->to_slave[p->ts_head] = buf[i];
            p->ts_head = (p->ts_head + 1) % PTY_BUF_SIZE;
        }
        wake_slave(p);
        return (int)n;
    }

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
        if (got > 0) {
            /* 唤醒等写 to_master 的 slave */
            if (p->wait_slave_write) {
                struct task *t = p->wait_slave_write;
                p->wait_slave_write = NULL;
                unblock_task(t, TASK_STATE_READY);
            }
            return got;
        }

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
        /* 唤醒等写的 */
        if (p->wait_slave_write) {
            struct task *w = p->wait_slave_write;
            p->wait_slave_write = NULL;
            unblock_task(w, TASK_STATE_READY);
        }
        if (p->refcount > 0) p->refcount--;
        if (p->refcount == 0) p->used = 0;
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

    int is_master = (cur->pty_handles[h].type == PTY_MASTER);

    uint32_t done = 0;
    uint8_t tmp[512];
    while (done < n) {
        uint32_t chunk = n - done;
        if (chunk > sizeof(tmp)) chunk = sizeof(tmp);
        if (copy_from_user(tmp, (uint32_t)buf_user + done, chunk) < 0)
            return EFAULT;

        int w = is_master
                ? pty_write_master(p, tmp, chunk)
                : pty_write_slave(p, tmp, chunk);
        if (w < 0) return done > 0 ? (int)done : w;
        if (w == 0) break;
        done += w;
    }
    return (int)done;
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

int sys_pty_avail(int h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;

    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;

    if (cur->pty_handles[h].type == PTY_MASTER)
        return (int)buf_count(p->tm_head, p->tm_tail);
    else
        return (int)buf_count(p->ts_head, p->ts_tail);
}

int sys_pty_set_winsize(int h, uint16_t cols, uint16_t rows) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;
    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;
    p->cols = cols;
    p->rows = rows;
    return OK;
}

int sys_term_set_raw(int raw) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    struct io_slot *slot = &cur->stdout_slot;
    if (slot->type != IO_SLOT_PTY_MASTER &&
        slot->type != IO_SLOT_PTY_SLAVE)
        return EINVAL;
    int h = slot->fd;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;
    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;
    p->raw = raw ? 1 : 0;
    return OK;
}

int sys_term_get_winsize(uint32_t *user_out) {
    struct task *cur = get_current_task();
    if (!cur || !user_out) return EINVAL;
    if (check_user_range((uint32_t)user_out, 8) < 0) return EFAULT;

    struct io_slot *slot = &cur->stdout_slot;
    if (slot->type != IO_SLOT_PTY_MASTER &&
        slot->type != IO_SLOT_PTY_SLAVE)
        return EINVAL;
    int h = slot->fd;
    if (h < 0 || h >= PTY_MAX_HANDLES) return EINVAL;
    if (!cur->pty_handles[h].used) return EINVAL;
    struct pty_pair *p = cur->pty_handles[h].pair;
    if (!p) return EINVAL;

    uint32_t out[2] = { p->cols, p->rows };
    if (copy_to_user((uint32_t)user_out, out, 8) < 0) return EFAULT;
    return OK;
}