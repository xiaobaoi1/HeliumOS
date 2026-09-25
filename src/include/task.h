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

struct task {
    uint32_t pid;
    uint32_t state;
    uint32_t time_slice;
    uint32_t *pgd;
    uint32_t kernel_stack_phys;
    uint32_t kernel_esp;
    uint32_t user_stack_phys;
    uint32_t user_stack_virt;
    uint32_t entry_point;
    struct task *next;
    uint32_t creator_pid;            /* ← parent 改成 creator_pid */
    int exit_status;

    uint32_t heap_base;
    uint32_t heap_brk;
    uint32_t heap_limit;

    uint32_t sleep_ticks;
    struct task *wait_target;
    uint32_t wait_deadline;          /* ← 新增：超时时间戳（tick） */

    char cwd_volume[VOL_NAME_LEN];
    char cwd_path[PATH_MAX_LEN];

    struct fs_handle  fs_handles[FS_MAX_HANDLES];
    struct dev_handle dev_handles[DEV_MAX_HANDLES];

    /* 进程对象（批次 D） */
    uint32_t refcount;               /* ← 新增 */
    uint8_t  zombie;                 /* ← 新增 */
    struct task *proc_next;          /* ← 新增：全局链表 */
    struct task *grave_next;
    struct proc_handle proc_handles[PROC_MAX_HANDLES];  /* ← 新增 */

    uint32_t syscall_regs;   /* ★ 最近一次 int 0x80 的 regs 指针 */
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