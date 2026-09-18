#include <scheduler.h>
#include <task.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

/* 处理睡眠：遍历 blocked_list，递减 SLEEPING 进程的 sleep_ticks */
void sleep_tick(void) {
    for (struct task *t = blocked_list_head; t; ) {
        struct task *next = t->next;
        if (t->state == TASK_STATE_SLEEPING && t->sleep_ticks > 0) {
            t->sleep_ticks--;
            if (t->sleep_ticks == 0) {
                unblock_task(t, TASK_STATE_READY);
            }
        }
        t = next;
    }
}

void schedule(void) {
    struct task *current = get_current_task();

    /* 当前进程仍可运行 */
    if (current && current->time_slice > 0 && current->state == TASK_STATE_RUNNING) {
        return;
    }

    /* 时间片用完，放回就绪队列 */
    if (current && current->state == TASK_STATE_RUNNING) {
        current->time_slice = TIME_SLICE_TICKS;
        current->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, current);
    }

    /* 取下一个 */
    struct task *next = dequeue_task(&ready_queue_head, &ready_queue_tail);
    if (!next) {
        kprintf("[SCHED] Idling...\n");
        set_current_task(NULL);
        while (1) __asm__("hlt");
        return;
    }

    set_current_task(next);
    switch_to(current, next);
}