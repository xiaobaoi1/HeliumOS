#include "sys/select.h"
#include "syscall.h"
#include "stdio.h"

int select(int nfds, fd_set *rfds, fd_set *wfds, fd_set *efds,
           struct timeval *timeout) {
    int ready = 0;

    if (rfds) {
        fd_set out;
        FD_ZERO(&out);
        for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
            if (!FD_ISSET(fd, rfds)) continue;
            int avail = (fd == 0) ? sys_fd_avail(0) : 0;
            if (avail > 0) { FD_SET(fd, &out); ready++; }
        }
        *rfds = out;
    }
    if (wfds) FD_ZERO(wfds);
    if (efds) FD_ZERO(efds);
    if (ready > 0) return ready;

    int ms = 100;
    if (timeout) {
        ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
        if (ms <= 0) return 0;
        if (ms > 100) ms = 100;
    }
    sys_sleep_ms(ms);
    return 0;
}