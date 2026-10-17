#include "termios.h"
#include "syscall.h"

int tcgetattr(int fd, struct termios *t) {
    if (!t) return -1;
    if (fd != 0 && fd != 1) return -1;

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
    if (fd != 0 && fd != 1) return -1;

    int raw = !(t->c_lflag & (ICANON | ECHO | ISIG));
    return term_set_raw(raw);
}