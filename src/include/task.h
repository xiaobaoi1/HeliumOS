#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_STATE_READY   0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2

/* 时间片（100Hz 下 10 个 tick = 100ms） */
#define TIME_SLICE_TICKS 10

extern struct task *current_task;

struct task {
    uint32_t pid;               /* +0  */
    uint32_t state;             /* +4  */
    uint32_t time_slice;        /* +8  */
    uint32_t *pgd;              /* +12 */
    uint32_t kernel_stack_phys; /* +16 */
    uint32_t kernel_esp;        /* +20 内核栈指针（上下文切换时保存） */
    uint32_t user_stack_phys;   /* +24 */
    uint32_t user_stack_virt;   /* +28 */
    uint32_t entry_point;       /* +32 */
    struct task *next;          /* +36 */
};

/* 创建进程（加载 ELF 后调用） */
struct task *task_create(uint32_t entry_point, uint32_t *pgd);

/* 当前进程 */
struct task *get_current_task(void);
void set_current_task(struct task *task);

/* 启动调度器（永不返回） */
void scheduler_start(void) __attribute__((noreturn));

void enqueue_task(struct task *task);
struct task *dequeue_task(void);

/* 进程退出（释放资源） */
void task_exit(struct task *task);

#endif