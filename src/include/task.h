#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <volume.h>
#include <path.h>
#include <fs.h>
#include <device.h>
#include <proc.h>
#include <signal.h>
#include <ipc.h>

/* ---------- I/O slot（进程的标准流端点） ---------- */

#define IO_SLOT_DEFAULT   0   /* 用内核默认（tty） */
#define IO_SLOT_FILE      1   /* 绑到一个 fs_handle */
#define IO_SLOT_NULL      2   /* 丢弃 */
#define IO_SLOT_PIPE_READ   3
#define IO_SLOT_PIPE_WRITE  4

struct io_slot {
    uint8_t  type;
    uint8_t  reserved[3];
    int      fd;              /* IO_SLOT_FILE 时有效；其他为 -1 */
};

/* ---------- 进程 spawn 时的重定向参数 ---------- */
#define SPAWN_FD_INHERIT   (-1)
#define SPAWN_FD_NULL      (-2)
#define SPAWN_FD_TTY       (-3)

/* 进程状态 */
#define TASK_STATE_READY         0   /* 就绪 */
#define TASK_STATE_RUNNING       1   /* 运行中 */
#define TASK_STATE_WAITING_CHILD 2   /* 等子进程退出（waitpid） */
#define TASK_STATE_SLEEPING      3   /* 睡眠中（sleep） */
#define TASK_STATE_ZOMBIE        4   /* 已退出，等父进程回收 */
#define TASK_STATE_WAITING_PIPE  5

#define TIME_SLICE_TICKS 10

/* 传参 */
#define ARGV_MAX  16
#define ARG_MAX   256

#define ENVP_MAX  16
#define ENV_MAX   256

/* 就绪队列（在 task.c 中定义） */
extern struct task *ready_queue_head;
extern struct task *ready_queue_tail;

/* 阻塞链表：WAITING_CHILD / SLEEPING / ZOMBIE 都挂在这里 */
extern struct task *blocked_list_head;
extern struct task *blocked_list_tail;

extern struct task *current_task;

/* 字段偏移约定（与 start.asm 的 switch_to 强耦合）：
 *   offset 0  = pid
 *   offset 4  = state
 *   offset 8  = time_slice
 *   offset 12 = pgd
 *   offset 16 = kernel_stack_phys
 *   offset 20 = kernel_esp
 * 修改前 6 个字段的顺序，必须同步修改 start.asm 的 switch_to。
 */
struct task {
    uint32_t pid;
    uint32_t state;
    uint32_t time_slice;
    uint32_t *pgd;
    uint32_t kernel_stack_phys;
        /* kernel_esp: 最近一次切走时的栈位置。
     * 语义：
     *   - 首次调度前，由 task_create 构造为"callee-saved 起始位置"
     *   - 每次 switch_to 保存 prev 时，写入 schedule 栈帧的位置
     *   - 被恢复时，switch_to 从这里加载 esp，pop callee-saved 后 ret
     * 不再由入口 stub 维护。
     */
    uint32_t kernel_esp;
    uint32_t user_stack_virt;
    uint32_t entry_point;
    struct task *next;
    uint32_t creator_pid;            /* 创建者的 pid，仅记录用 */
    int exit_status;

    uint32_t heap_base;
    uint32_t heap_brk;
    uint32_t heap_limit;

    uint32_t sleep_ticks;
    struct task *wait_target;
    uint32_t wait_deadline;          /* WAITING_CHILD 的超时时刻（tick） */

    char cwd_volume[VOL_NAME_LEN];
    char cwd_path[PATH_MAX_LEN];

    struct fs_handle  fs_handles[FS_MAX_HANDLES];
    struct dev_handle dev_handles[DEV_MAX_HANDLES];

    /* ---------- 进程对象模型 ---------- */
    uint32_t refcount;               /* self 引用 + 每个 proc_handles[i] */
    uint8_t  zombie;                 /* 1 = 已退出，等回收 */
    struct task *proc_next;          /* 全局进程链表 next */
    struct task *grave_next;         /* graveyard 链表 next */
    struct proc_handle proc_handles[PROC_MAX_HANDLES];

    struct io_slot stdin_slot;
    struct io_slot stdout_slot;
    struct io_slot stderr_slot;

    /* ---------- 信号 ---------- */
    uint32_t pending_signals;            /* 位图：bit N = 信号 N pending */
    uint32_t blocked_signals;            /* 信号掩码 */
    struct sig_action sig_actions[32];   /* 每信号的 handler */

    /* ---------- 状态字段扩展 ---------- */
    struct pipe *wait_pipe;   /* WAITING_PIPE 时有效 */

    /* ---------- IPC ---------- */
    struct ipc_handle ipc_handles[IPC_MAX_HANDLES];
};

void enqueue_task(struct task **head, struct task **tail, struct task *task);
struct task *dequeue_task(struct task **head, struct task **tail);
void remove_task_from_queue(struct task **head, struct task **tail, struct task *task);

/* 阻塞/唤醒 */
void block_current(uint32_t new_state);
void unblock_task(struct task *t, uint32_t new_state);
void wake_up_waiters(struct task *target);

struct task *task_create(uint32_t entry_point, uint32_t *pgd,
                         int argc, char *const argv[],
                         int envc, char *const envp[],
                         int redir_in_fd, int redir_out_fd, int redir_err_fd,
                         int redir_in_pipe, int redir_out_pipe, int redir_err_pipe);
struct task *get_current_task(void);
void set_current_task(struct task *task);
void scheduler_start(void) __attribute__((noreturn));
void task_exit(struct task *task, int status);

/* 强制终止进程（不释放 self 引用——由调用者决定）。
 * 语义：
 *   - 标记 zombie
 *   - 从所有队列移除，挂到 blocked_list
 *   - 唤醒等待者
 *   - 如果它是键盘前台，恢复父进程为前台
 * 被 kill 或用户异常时使用。 */
void task_terminate(struct task *t, int status);

#endif