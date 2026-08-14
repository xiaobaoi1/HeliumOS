#ifndef TASK_H
#define TASK_H

#include <stdint.h>

#define TASK_STATE_READY  0
#define TASK_STATE_RUNNING 1
#define TASK_STATE_BLOCKED 2

struct task {
    uint32_t pid;
    uint32_t state;
    uint32_t *pgd;                  /* 页目录虚拟地址（物理地址可直接访问） */
    uint32_t kernel_stack_phys;     /* 内核栈物理地址 */
    uint32_t user_stack_phys;       /* 用户栈物理地址 */
    uint32_t user_stack_virt;       /* 用户栈虚拟地址（0x7FFFE000） */
    uint32_t entry_point;           /* 用户程序入口（虚拟地址） */
    struct task *next;
};

void task_init(void);
struct task *task_create(uint32_t entry_point, uint32_t *pgd);
void task_switch_to(struct task *task);
void task_run_first(struct task *task);

#endif