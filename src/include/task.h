#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include <volume.h>
#include <path.h>
#include <fs.h>
#include <device.h>
#include <proc.h>

/* 进程状态 */
#define TASK_STATE_READY         0   /* 就绪 */
#define TASK_STATE_RUNNING       1   /* 运行中 */
#define TASK_STATE_WAITING_CHILD 2   /* 等子进程退出（waitpid） */
#define TASK_STATE_SLEEPING      3   /* 睡眠中（sleep） */
#define TASK_STATE_ZOMBIE        4   /* 已退出，等父进程回收 */

#define TIME_SLICE_TICKS 10

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
    uint32_t user_stack_phys;        /* 仅记录，实际释放通过 pgd 遍历 */
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
};

void enqueue_task(struct task **head, struct task **tail, struct task *task);
struct task *dequeue_task(struct task **head, struct task **tail);
void remove_task_from_queue(struct task **head, struct task **tail, struct task *task);

/* 阻塞/唤醒 */
void block_current(uint32_t new_state);
void unblock_task(struct task *t, uint32_t new_state);
void wake_up_waiters(struct task *target);

struct task *task_create(uint32_t entry_point, uint32_t *pgd);
struct task *get_current_task(void);
void set_current_task(struct task *task);
void scheduler_start(void) __attribute__((noreturn));
void task_exit(struct task *task, int status);

#endif