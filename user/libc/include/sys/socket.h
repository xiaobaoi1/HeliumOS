#ifndef USER_SYS_SOCKET_H
#define USER_SYS_SOCKET_H

#include <stdint.h>
#include "syscall.h"


#define SOCK_STREAM 1
#define AF_INET     2
#define SOCK_DGRAM  2
#define SOCK_ICMP   3

struct sockaddr {
    unsigned short sa_family;
    char           sa_data[14];
};

struct sockaddr_in {
    unsigned short sin_family;
    unsigned short sin_port;    /* 网络字节序 */
    unsigned int   sin_addr;    /* 网络字节序 */
    char           sin_zero[8];
};

static inline int socket(int domain, int type, int proto) {
    return __syscall(SYS_SOCKET, domain, type, proto);
}

static inline int bind(int h, const struct sockaddr *addr,
                       unsigned int addrlen) {
    return __syscall(SYS_BIND, h, (int)addr, (int)addrlen);
}

static inline int sendto(int h, const void *buf, unsigned int len,
                         const struct sockaddr *dst, unsigned int addrlen) {
    return __syscall5(SYS_SENDTO, h, (int)buf, (int)len,
                      (int)dst, (int)addrlen);
}

static inline int recvfrom(int h, void *buf, unsigned int len,
                           struct sockaddr *src, unsigned int *addrlen) {
    return __syscall5(SYS_RECVFROM, h, (int)buf, (int)len,
                      (int)src, (int)addrlen);
}

static inline int sockclose(int h) {
    return __syscall(SYS_SOCKCLOSE, h, 0, 0);
}

static inline int connect(int h, const struct sockaddr *addr,
                          unsigned int addrlen) {
    return __syscall(SYS_CONNECT, h, (int)addr, (int)addrlen);
}

static inline int listen(int h, int backlog) {
    return __syscall(SYS_LISTEN, h, backlog, 0);
}

static inline int accept(int h, struct sockaddr *addr, unsigned int *addrlen) {
    return __syscall(SYS_ACCEPT, h, (int)addr, (int)addrlen);
}

#endif