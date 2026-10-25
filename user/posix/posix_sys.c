#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <stdarg.h>
#include "posix_internal.h"
#include <stddef.h>

/* 避开 syscall.h 里的 static inline getcwd(char*, int) */
#define getcwd sys_getcwd_stub
#include "syscall.h"
#undef getcwd

/* 导出标准 POSIX 的 getcwd 符号供 TCC 链接 */
char *getcwd(char *buf, size_t size) {
    if (__syscall(SYS_GETCWD, (int)buf, (int)size, 0) < 0) {
        return 0;
    }
    return buf;
}

int isatty(int fd);

int ioctl(int fd, unsigned long req, ...) {
    va_list ap;
    va_start(ap, req);
    
    if (req == TIOCGWINSZ) {
        struct winsize *ws = va_arg(ap, struct winsize*);
        unsigned int out[2];
        if (term_get_winsize(out) == 0) {
            ws->ws_col = out[0];
            ws->ws_row = out[1];
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
            va_end(ap);
            return 0;
        }
    }
    va_end(ap);
    return -1;
}

int tcgetattr(int fd, struct termios *t) {
    if (!t) return -1;
    t->c_iflag = 0;
    t->c_oflag = 0;
    t->c_cflag = 0;
    t->c_lflag = ICANON | ECHO | ISIG;
    for (int i = 0; i < 32; i++) t->c_cc[i] = 0;
    return 0;
}

int tcsetattr(int fd, int opt, const struct termios *t) {
    (void)opt;
    if (!t) return -1;
    int raw = !(t->c_lflag & (ICANON | ECHO | ISIG));
    return term_set_raw(raw);
}

time_t time(time_t *t) {
    if (t) *t = 0;
    return 0;
}

int isatty(int fd) {
    (void)fd;
    return 1;
}

struct tm *localtime(const time_t *timep) {
    static struct tm dummy;
    (void)timep;
    return &dummy;
}

char *strerror(int errnum) {
    (void)errnum;
    return "error";
}

int gettimeofday(void *tv, void *tz) {
    (void)tv; (void)tz;
    return 0;
}

void execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    /* 不支持 */
}

int realpath(const char *path, char *resolved_path) {
    if (!path || !resolved_path) return -1;
    int i = 0;
    while (path[i]) { resolved_path[i] = path[i]; i++; }
    resolved_path[i] = '\0';
    return 0;
}

int remove(const char *path) {
    return fs_unlink(path); /* 或者用 unlink */
}

int unlink(const char *path) {
    return fs_unlink(path);
}