#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_STATE_READY   0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2
#define TASK_STATE_WAITING 3
#define TASK_STATE_ZOMBIE  4

#define TIME_SLICE_TICKS 10

/* 队列头（在 task.c 中定义） */
extern struct task *ready_queue_head;
extern struct task *ready_queue_tail;
extern struct task *waiting_queue_head;
extern struct task *waiting_queue_tail;
extern struct task *zombie_queue_head;
extern struct task *zombie_queue_tail;

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
    struct task *parent;
    int exit_status;
};

void enqueue_task(struct task **head, struct task **tail, struct task *task);
struct task *dequeue_task(struct task **head, struct task **tail);
void remove_task_from_queue(struct task **head, struct task **tail, struct task *task);

struct task *task_create(uint32_t entry_point, uint32_t *pgd);
struct task *get_current_task(void);
void set_current_task(struct task *task);
void scheduler_start(void) __attribute__((noreturn));
void task_exit(struct task *task, int status);

#endif