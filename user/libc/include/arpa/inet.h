#ifndef USER_ARPA_INET_H
#define USER_ARPA_INET_H

#include <stdint.h>

/* 32 位 IPv4 地址容器 */
struct in_addr {
    uint32_t s_addr;    /* 网络字节序 */
};

uint32_t inet_addr(const char *cp);
char    *inet_ntoa(uint32_t in);
int inet_aton(const char *cp, struct in_addr *inp);
int inet_pton(int af, const char *src, void *dst);

static inline unsigned int htonl(unsigned int x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline unsigned short htons(unsigned short x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}
static inline unsigned int   ntohl(unsigned int x)   { return htonl(x); }
static inline unsigned short ntohs(unsigned short x) { return htons(x); }

#endif