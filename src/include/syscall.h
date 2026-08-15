#ifndef SYSCALL_H
#define SYSCALL_H

#include <isr.h>

/* 系统调用号（遵循 Linux 风格命名） */
#define SYS_WRITE 1
#define SYS_EXIT  2

/* 系统调用处理函数声明 */
void syscall_handler(struct registers *regs);

#endif