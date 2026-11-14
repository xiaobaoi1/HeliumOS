#ifndef USER_SYS_SELECT_H
#define USER_SYS_SELECT_H

#include <sys/time.h>

#define FD_SETSIZE 64

typedef struct {
    unsigned int __bits[(FD_SETSIZE + 31) / 32];
} fd_set;

#define FD_ZERO(s) do { \
    fd_set *__s = (s); \
    for (unsigned int __i = 0; __i < (FD_SETSIZE + 31) / 32; __i++) \
        __s->__bits[__i] = 0; \
} while (0)

#define FD_SET(fd, s) do { \
    int __fd = (fd); \
    if (__fd >= 0 && __fd < FD_SETSIZE) \
        (s)->__bits[__fd / 32] |= (1u << (__fd % 32)); \
} while (0)

#define FD_CLR(fd, s) do { \
    int __fd = (fd); \
    if (__fd >= 0 && __fd < FD_SETSIZE) \
        (s)->__bits[__fd / 32] &= ~(1u << (__fd % 32)); \
} while (0)

#define FD_ISSET(fd, s) ({ \
    int __fd = (fd); \
    int __r = 0; \
    if (__fd >= 0 && __fd < FD_SETSIZE) \
        __r = ((s)->__bits[__fd / 32] >> (__fd % 32)) & 1; \
    __r; \
})

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);

#endif