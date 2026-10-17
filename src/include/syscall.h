#ifndef SYSCALL_H
#define SYSCALL_H

#include <isr.h>

/* ========== 进程控制 (1~9) ========== */
#define SYS_EXIT          1
#define SYS_GETPID        2
#define SYS_SPAWN         3
#define SYS_GETPPID       4
#define SYS_SLEEP         5
#define SYS_WAIT          6
#define SYS_KILL          7
#define SYS_PROC_CLOSE    8
#define SYS_PROC_LIST     9
#define SYS_PROC_OPEN     10

/* ========== 标准流 (11~12) ========== */
#define SYS_READ                     11
#define SYS_WRITE                    12

/* ========== 内存管理 (21~29) ========== */
#define SYS_BRK        21
#define SYS_MMAP       22
#define SYS_MUNMAP     23
#define SYS_SHM_OPEN   24
#define SYS_SHM_UNLINK 25

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
#define SYS_FS_FSTAT      43
#define SYS_FS_TRUNCATE   44

/* ========== 设备 (51~59) ========== */
#define SYS_DEV_OPEN      51
#define SYS_DEV_READ      52
#define SYS_DEV_WRITE     53
#define SYS_DEV_IOCTL     54
#define SYS_DEV_CLOSE     55

/* ========== 进程环境 (61~69) ========== */
#define SYS_GETCWD        61
#define SYS_CHDIR         62
#define SYS_UNAME         63

/* ========== 信号 (71~79) ========== */
#define SYS_SIGACTION     71
#define SYS_SIGRETURN     72
#define SYS_SIGPROCMASK   73

/* ========== IPC (81~89) ========== */
#define SYS_PIPE         81
#define SYS_PIPE_READ    82
#define SYS_PIPE_WRITE   83
#define SYS_IPC_CLOSE    84
#define SYS_PIPE_NAMED   85
#define SYS_PIPE_OPEN    86
#define SYS_PIPE_UNLINK  87
#define SYS_PIPE_AVAIL  88

/* ========== 系统控制 (91~99) ========== */
#define SYS_RTC_GET_TIME  91
#define SYS_REBOOT        92

/* ========== 网络 (100~109) ========== */
#define SYS_SOCKET      100
#define SYS_BIND        101
#define SYS_SENDTO      102
#define SYS_RECVFROM    103
#define SYS_SOCKCLOSE   104
#define SYS_CONNECT     105
#define SYS_LISTEN      106
#define SYS_ACCEPT      107

/* ========== 输入 / 显示 (130~139) ========== */
#define SYS_INPUT_POLL    130
#define SYS_INPUT_CLAIM   131
#define SYS_FB_MAP        132
#define SYS_FB_CLAIM      133

/* ========== pty (140~149) ========== */
#define SYS_PTY_OPEN    140
#define SYS_PTY_READ    141
#define SYS_PTY_WRITE   142
#define SYS_PTY_CLOSE   143
#define SYS_PTY_SET_FG  144
#define SYS_PTY_AVAIL   145
#define SYS_FD_AVAIL    146
#define SYS_PTY_SET_WINSIZE 147
#define SYS_TERM_SET_RAW    148
#define SYS_TERM_GET_WINSIZE 149

/* ========== 网络配置 (150~159) ========== */
#define SYS_NET_SET_IF   150
#define SYS_NET_GET_IF   151
#define SYS_NET_SET_DNS  152
#define SYS_NET_GET_DNS  153




/* 标准流编号（sys_read / sys_write 的参数，不是文件 fd） */
#define STREAM_STDIN   0
#define STREAM_STDOUT  1
#define STREAM_STDERR  2


/* 系统调用处理函数声明 */
void syscall_handler(struct registers *regs);

#endif