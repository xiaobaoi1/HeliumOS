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
    kprintf("+");
    struct task *current = get_current_task();

    // 当前进程仍可运行（时间片未用完）
    if (current && current->time_slice > 0 && current->state == TASK_STATE_RUNNING) {
        return;
    }

    // 当前进程时间片用完，重置后放回 READY_QUEUE
    if (current && current->state == TASK_STATE_RUNNING) {
        current->time_slice = TIME_SLICE_TICKS;
        current->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, current);
    }

    // 从就绪队列取出下一个进程
    struct task *next = dequeue_task(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        kprintf("[SCHED] Idling...\n");
        set_current_task(NULL);
        return;
    }

    set_current_task(next);
    switch_to(current, next);
}