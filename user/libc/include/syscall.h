#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

/* ========== 进程控制 (1~9) ========== */
#define SYS_EXIT          1
#define SYS_GETPID        2
#define SYS_SPAWN         3
#define SYS_WAITPID       4
#define SYS_SLEEP         5
#define SYS_WAIT          6
#define SYS_KILL          7
#define SYS_PROC_CLOSE    8

/* ========== I/O 操作 (11~19) ========== */
#define SYS_READ                     11
#define SYS_WRITE                    12
#define SYS_CONSOLE_CLEAR            13
#define SYS_CONSOLE_SET_COLOR        14
#define SYS_CONSOLE_SET_CURSOR       15
#define SYS_CONSOLE_GET_CURSOR       16
#define SYS_CONSOLE_SAVE_CURSOR      17
#define SYS_CONSOLE_RESTORE_CURSOR   18
#define SYS_CONSOLE_DEBUG_WRITE      19

/* ========== 内存管理 (21~29) ========== */
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

/* ========== 设备 (41~49) ========== */
#define SYS_DEV_OPEN      41
#define SYS_DEV_READ      42
#define SYS_DEV_WRITE     43
#define SYS_DEV_IOCTL     44
#define SYS_DEV_CLOSE     45

/* ========== 进程环境 (61~69) ========== */
#define SYS_GETCWD        61
#define SYS_CHDIR         62

/* ---------- 原始系统调用（内联汇编） ---------- */
static inline int __syscall(int num, int a, int b, int c) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(a), "c"(b), "d"(c)
        : "memory", "cc"
    );
    return ret;
}

/* ---------- 便捷包装 ---------- */

/* 进程控制 */
static inline void _exit(int status) {
    __syscall(SYS_EXIT, status, 0, 0);
    for (;;) {}   /* 不会返回 */
}

static inline int getpid(void) {
    return __syscall(SYS_GETPID, 0, 0, 0);
}

/* 进程控制 */
static inline int spawn(const char *path, char *const argv[]) {
    return __syscall(SYS_SPAWN, (int)path, (int)argv, 0);
}

static inline int waitpid(int pid, int *status) {
    return __syscall(SYS_WAITPID, pid, (int)status, 0);
}

static inline int wait(int handle, int *status, unsigned int timeout_ms) {
    return __syscall(SYS_WAIT, handle, (int)status, (int)timeout_ms);
}

static inline int kill(int handle, int status) {
    return __syscall(SYS_KILL, handle, status, 0);
}

static inline int process_close(int handle) {
    return __syscall(SYS_PROC_CLOSE, handle, 0, 0);
}

static inline void sleep_ms(int ms) {
    __syscall(SYS_SLEEP, ms, 0, 0);
}

/* 控制台 I/O */
static inline int write(int fd, const void *buf, unsigned int n) {
    return __syscall(SYS_WRITE, fd, (int)buf, (int)n);
}

static inline int read(int fd, void *buf, unsigned int n) {
    return __syscall(SYS_READ, fd, (int)buf, (int)n);
}

static inline void console_clear(void) {
    __syscall(SYS_CONSOLE_CLEAR, 0, 0, 0);
}

static inline void console_set_color(unsigned char fg, unsigned char bg) {
    __syscall(SYS_CONSOLE_SET_COLOR, fg, bg, 0);
}

static inline void console_set_cursor(int x, int y) {
    __syscall(SYS_CONSOLE_SET_CURSOR, x, y, 0);
}

static inline void console_get_cursor(int *x, int *y) {
    int out[2];
    __syscall(SYS_CONSOLE_GET_CURSOR, (int)out, 0, 0);
    if (x) *x = out[0];
    if (y) *y = out[1];
}

static inline void console_save_cursor(void) {
    __syscall(SYS_CONSOLE_SAVE_CURSOR, 0, 0, 0);
}

static inline void console_restore_cursor(void) {
    __syscall(SYS_CONSOLE_RESTORE_CURSOR, 0, 0, 0);
}

static inline int console_debug_write(const void *buf, unsigned int n) {
    return __syscall(SYS_CONSOLE_DEBUG_WRITE, (int)buf, (int)n, 0);
}

/* 内存 */
static inline int brk(int new_brk) {
    return __syscall(SYS_BRK, new_brk, 0, 0);
}

/* 文件系统 */
static inline int fs_open(const char *path, int flags) {
    return __syscall(SYS_FS_OPEN, (int)path, flags, 0);
}

static inline int fs_read(int fd, void *buf, unsigned int n) {
    return __syscall(SYS_FS_READ, fd, (int)buf, (int)n);
}

static inline int fs_write(int fd, const void *buf, unsigned int n) {
    return __syscall(SYS_FS_WRITE, fd, (int)buf, (int)n);
}

static inline int fs_seek(int fd, unsigned int offset) {
    return __syscall(SYS_FS_SEEK, fd, (int)offset, 0);
}

static inline int fs_close(int fd) {
    return __syscall(SYS_FS_CLOSE, fd, 0, 0);
}

static inline int fs_opendir(const char *path) {
    return __syscall(SYS_FS_OPENDIR, (int)path, 0, 0);
}

static inline int fs_readdir(int fd, void *ent) {
    return __syscall(SYS_FS_READDIR, fd, (int)ent, 0);
}

static inline int fs_closedir(int fd) {
    return __syscall(SYS_FS_CLOSEDIR, fd, 0, 0);
}

static inline int getcwd(char *buf, int size) {
    return __syscall(SYS_GETCWD, (int)buf, size, 0);
}

static inline int chdir(const char *path) {
    return __syscall(SYS_CHDIR, (int)path, 0, 0);
}

/* 设备 */
static inline int dev_open(int type, void *arg) {
    return __syscall(SYS_DEV_OPEN, type, (int)arg, 0);
}

static inline int dev_read(int dev, void *buf, unsigned int n) {
    return __syscall(SYS_DEV_READ, dev, (int)buf, (int)n);
}

static inline int dev_write(int dev, const void *buf, unsigned int n) {
    return __syscall(SYS_DEV_WRITE, dev, (int)buf, (int)n);
}

static inline int dev_close(int dev) {
    return __syscall(SYS_DEV_CLOSE, dev, 0, 0);
}

/* ---------- 打开标志 ---------- */
#define FS_O_RDONLY   0x01
#define FS_O_WRONLY   0x02
#define FS_O_RDWR     0x03
#define FS_O_CREAT    0x04

/* ---------- dirent（和内核一致） ---------- */
#define DIRENT_NAME_MAX  256
struct dirent {
    char          name[DIRENT_NAME_MAX];
    unsigned char attributes;
    unsigned char reserved[3];
    unsigned int  size;
};

/* ---------- 标准 fd ---------- */
#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

/* 设备类型 */
#define DEV_TYPE_KEYBOARD  1
#define DEV_TYPE_SERIAL    2
#define DEV_TYPE_VGA       3
#define DEV_TYPE_ATA       4

#endif