#ifndef SYSCALL_H
#define SYSCALL_H

#include <isr.h>

/* ========== 进程控制 (1~9) ========== */
#define SYS_EXIT          1
#define SYS_GETPID        2
#define SYS_SPAWN         3      /* 返回 proc_handle_t（不再是 pid） */

#define SYS_SLEEP         5
#define SYS_WAIT          6      /* 新增：wait(handle, *status, timeout) */
#define SYS_KILL          7      /* 新增：kill(handle, status) */
#define SYS_PROC_CLOSE    8      /* 新增：process_close(handle) */

/* ========== 标准流 (11~12) ========== */
#define SYS_READ                     11
#define SYS_WRITE                    12

/* ========== tty 控制 (13~19) ========== */
#define SYS_TTY_CLEAR                13
#define SYS_TTY_SET_COLOR            14
#define SYS_TTY_SET_CURSOR           15
#define SYS_TTY_GET_CURSOR           16
#define SYS_TTY_SAVE_CURSOR          17
#define SYS_TTY_RESTORE_CURSOR       18
#define SYS_TTY_DEBUG_WRITE          19
#define SYS_TTY_SET_FOREGROUND       20

/* ========== 内存管理 (21~29) ========== */
#define SYS_BRK        21

/* ========== 文件系统 (31~49) ========== */
#define SYS_FS_OPEN       31
#define SYS_FS_READ       32
#define SYS_FS_WRITE      33
#define SYS_FS_SEEK       34
#define SYS_FS_CLOSE      35
#define SYS_FS_OPENDIR    36
#define SYS_FS_READDIR    37
#define SYS_FS_CLOSEDIR   38
#define SYS_FS_UNLINK     39
#define SYS_FS_MKDIR      40
#define SYS_FS_RMDIR      41
#define SYS_FS_RENAME     42

/* ========== 设备 (51~59) ========== */
#define SYS_DEV_OPEN      51
#define SYS_DEV_READ      52
#define SYS_DEV_WRITE     53
#define SYS_DEV_IOCTL     54
#define SYS_DEV_CLOSE     55

/* ========== 进程环境 (61~69) ========== */
#define SYS_GETCWD        61
#define SYS_CHDIR         62

/* ========== 信号 (71~79) ========== */
#define SYS_SIGACTION     71
#define SYS_SIGRETURN     72
#define SYS_SIGPROCMASK   73

/* ========== IPC (81~89) ========== */
#define SYS_PIPE         81
#define SYS_PIPE_READ    82
#define SYS_PIPE_WRITE   83
#define SYS_IPC_CLOSE    84

/* ========== 系统控制 (91~99) ========== */
#define SYS_RTC_GET_TIME  91
#define SYS_REBOOT        92

/* 系统调用处理函数声明 */
void syscall_handler(struct registers *regs);

#endif