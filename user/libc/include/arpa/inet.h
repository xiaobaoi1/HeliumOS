#ifndef USER_ARPA_INET_H
#define USER_ARPA_INET_H

#include <stdint.h>

/* 32 位 IPv4 地址容器 */
struct in_addr {
    uint32_t s_addr;    /* 网络字节序 */
};

uint32_t inet_addr(const char *cp);
char    *inet_ntoa(uint32_t in);

#endif