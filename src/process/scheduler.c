#include <scheduler.h>
#include <task.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

/* 外部引用就绪队列（在 task.c 中） */
// extern struct task *get_ready_queue_head(void);
// extern struct task *get_ready_queue_tail(void);
// extern void set_ready_queue_head(struct task *h);
// extern void set_ready_queue_tail(struct task *t);

/* 队列操作（直接操作全局变量） */
// void enqueue_task(struct task *task) {
//     task->state = TASK_STATE_READY;
//     task->next = NULL;
//     struct task *tail = get_ready_queue_tail();
//     if (tail) {
//         tail->next = task;
//         set_ready_queue_tail(task);
//     } else {
//         set_ready_queue_head(task);
//         set_ready_queue_tail(task);
//     }
// }

// struct task *dequeue_task(void) {
//     struct task *head = get_ready_queue_head();
//     if (!head) return NULL;
//     set_ready_queue_head(head->next);
//     if (!get_ready_queue_head()) {
//         set_ready_queue_tail(NULL);
//     }
//     head->next = NULL;
//     return head;
// }

/* 调度核心 */
void schedule(void) {
    struct task *current = get_current_task();
    if (!current) return;

    /* 时间片还有，不调度 */
    if (current->time_slice > 0) return;

    /* 重置时间片 */
    current->time_slice = TIME_SLICE_TICKS;

    /* 将当前进程放回就绪队列 */
    enqueue_task(current);

    /* 取出下一个进程 */
    struct task *next = dequeue_task();
    if (!next) {
        /* 没有其他进程，继续当前进程 */
        set_current_task(current);
        return;
    }

    /* 如果下一个就是当前进程，不切换 */
    if (next == current) {
        set_current_task(current);
        return;
    }

    /* 切换到下一个进程 */
    set_current_task(next);
    switch_to(current, next);
}