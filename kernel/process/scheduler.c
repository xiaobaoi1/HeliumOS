#include <scheduler.h>
#include <task.h>
#include <tss.h>
#include <printf.h>
#include <stddef.h>

static uint64_t g_ticks = 0;

uint64_t get_ticks(void) { return g_ticks; }

/* 处理睡眠：遍历 blocked_list，递减 SLEEPING 进程的 sleep_ticks */
void sleep_tick(void) {
    g_ticks++;

    for (struct task *t = blocked_list_head; t; ) {
        struct task *next = t->next;

        /* SLEEPING 递减 */
        if (t->state == TASK_STATE_SLEEPING && t->sleep_ticks > 0) {
            t->sleep_ticks--;
            if (t->sleep_ticks == 0) {
                unblock_task(t, TASK_STATE_READY);
            }
        }

        /* WAITING_CHILD 检查超时 */
        if (t->state == TASK_STATE_WAITING_CHILD &&
            t->wait_deadline > 0 &&
            g_ticks >= t->wait_deadline) {
            unblock_task(t, TASK_STATE_READY);
            /* wait 函数唤醒后会自己检查超时 */
        }

        t = next;
    }
}

/* 防止嵌套 schedule 把 current 消费掉。
 * schedule 只允许在栈最外层真正切换；
 * 中断里的 schedule 如果看到 g_in_schedule != 0，直接返回，
 * 由外层 schedule 的 for 循环处理。 */
void schedule(void) {
    struct task *current = get_current_task();

    /* 已经在该进程的 schedule 栈帧里（hlt 期间中断重入），不再调度。
     * 用 current->in_schedule 而非全局标志，避免跨进程泄漏。 */
    if (current && current->in_schedule) return;

    if (current && current->time_slice > 0 && current->state == TASK_STATE_RUNNING) {
        return;
    }

    if (current) current->in_schedule = 1;

    if (current && current->state == TASK_STATE_RUNNING) {
        current->time_slice = TIME_SLICE_TICKS;
        current->state = TASK_STATE_READY;
        enqueue_task(&ready_queue_head, &ready_queue_tail, current);
    }

    for (;;) {
        struct task *next = dequeue_task(&ready_queue_head, &ready_queue_tail);
        if (next) {
            if (next == current) {
                /* 自己继续跑，不切 */
                next->state = TASK_STATE_RUNNING;
                next->time_slice = TIME_SLICE_TICKS;
                next->in_schedule = 0;
                return;
            }
            set_current_task(next);
            switch_to(current, next);

            /* 被切回来：从 switch_to 之后继续。
             * current 是全局 current_task，此时是切回来的进程。 */
            current = get_current_task();
            if (current) current->in_schedule = 0;
            return;
        }

        /* 无进程可运行：开中断休眠，等时钟 tick 唤醒。
         * 中断里的 schedule 看到 current->in_schedule=1 直接返回，
         * 不会抢走 ready queue。 */
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}