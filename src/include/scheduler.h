#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <task.h>

/* 在时钟中断中调用，进行调度 */
void schedule(void);

/* 上下文切换（汇编实现） */
void switch_to(struct task *prev, struct task *next);

#endif