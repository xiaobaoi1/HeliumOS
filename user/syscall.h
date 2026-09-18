#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

/* 系统调用号（与内核保持一致） */
#define SYS_EXIT       1
#define SYS_GETPID     2
#define SYS_SPAWN      3
#define SYS_WAITPID    4
#define SYS_SLEEP      5

#define SYS_READ       11
#define SYS_WRITE      12

#define SYS_BRK        21

/* 声明汇编实现的 syscall 函数 */
int syscall(int num, int arg1, int arg2, int arg3);

#endif