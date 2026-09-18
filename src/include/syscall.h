#ifndef SYSCALL_H
#define SYSCALL_H

#include <isr.h>

/* ========== 进程控制 (1~9) ========== */
#define SYS_EXIT       1
#define SYS_GETPID     2
#define SYS_SPAWN      3
#define SYS_WAITPID    4
#define SYS_SLEEP      5   // 留作以后实现

/* ========== I/O 操作 (11~19) ========== */
#define SYS_READ       11
#define SYS_WRITE      12

/* ========== 内存管理 (21~29) ========== */
#define SYS_BRK        21

/* 系统调用处理函数声明 */
void syscall_handler(struct registers *regs);

#endif