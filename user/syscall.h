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

/* ========== 文件系统 (31~39) ========== */
#define SYS_FS_OPEN       31
#define SYS_FS_READ       32
#define SYS_FS_WRITE      33
#define SYS_FS_SEEK       34
#define SYS_FS_CLOSE      35
#define SYS_FS_OPENDIR    36
#define SYS_FS_READDIR    37
#define SYS_FS_CLOSEDIR   38

/* ========== 进程环境 (61~69) ========== */
#define SYS_GETCWD        61
#define SYS_CHDIR         62

/* 声明汇编实现的 syscall 函数 */
int syscall(int num, int arg1, int arg2, int arg3);

#endif