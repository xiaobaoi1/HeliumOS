#include <arpa/inet.h>
#include <stdio.h>
#include "syscall.h"    /* 需要 htonl / ntohl */

uint32_t inet_addr(const char *cp) {
    if (!cp) return 0;
    unsigned int v[4];
    const char *p = cp;
    for (int i = 0; i < 4; i++) {
        if (*p < '0' || *p > '9') return 0;
        unsigned int n = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            n = n * 10 + (*p - '0');
            p++;
            digits++;
            if (n > 255) return 0;
        }
        if (digits == 0) return 0;
        v[i] = n;
        if (i < 3) {
            if (*p != '.') return 0;
            p++;
        }
    }
    if (*p != '\0') return 0;

    /* 组装成"主机序的数值"，再 htonl 转网络序。
     * 这样返回值的内存字节 = IP 各字节顺序（网络字节序）。 */
    uint32_t host = (v[0] << 24) | (v[1] << 16) | (v[2] << 8) | v[3];
    return htonl(host);
}

char *inet_ntoa(uint32_t in) {
    static char buf[16];
    /* in 是网络字节序；ntohl 转成主机序后各字节对应 IP 各段 */
    uint32_t h = ntohl(in);
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (h >> 24) & 0xFF, (h >> 16) & 0xFF,
             (h >> 8) & 0xFF, h & 0xFF);
    return buf;
}