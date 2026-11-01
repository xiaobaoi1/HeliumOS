#include <sys/ioctl.h>
#include <stdarg.h>
#include "syscall.h"

int ioctl(int fd, unsigned long req, ...) {
    if (fd != 0 && fd != 1) return -1;

    if (req == TIOCGWINSZ) {
        va_list ap;
        va_start(ap, req);
        struct winsize *ws = va_arg(ap, struct winsize*);
        va_end(ap);
        if (!ws) return -1;

        unsigned int out[2];
        if (term_get_winsize(out) < 0) return -1;

        ws->ws_col = (unsigned short)out[0];
        ws->ws_row = (unsigned short)out[1];
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    return -1;
}