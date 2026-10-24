#include <sys/ioctl.h>
#include <termios.h>
#include <stdarg.h>
#include <time.h>
#include "syscall.h"

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