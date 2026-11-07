#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "stdio.h"
#include "string.h"

#define PING_ID  0x1234

static unsigned short csum16(const void *data, int len) {
    const unsigned char *p = data;
    unsigned int sum = 0;
    for (int i = 0; i + 1 < len; i += 2)
        sum += ((unsigned int)p[i] << 8) | p[i + 1];
    if (len & 1) sum += (unsigned int)p[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)(~sum);
}

/* 输入可能是 IP 也可能是域名。返回网络序 IP；失败返回 0。 */
static uint32_t resolve(const char *host) {
    uint32_t ip = inet_addr(host);
    if (ip != 0) return ip;

    struct hostent *h = gethostbyname(host);
    if (!h || !h->h_addr_list || !h->h_addr_list[0]) return 0;
    return *(uint32_t*)h->h_addr_list[0];
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: ping <host>\n"); return 1; }

    uint32_t dst_ip = resolve(argv[1]);
    if (dst_ip == 0) {
        printf("ping: cannot resolve '%s'\n", argv[1]);
        return 1;
    }

    printf("PING %s (%s)\n", argv[1], inet_ntoa(dst_ip));

    int s = socket(AF_INET, SOCK_ICMP, 0);
    if (s < 0) { printf("ping: socket error %d\n", s); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr = dst_ip;

    for (unsigned short seq = 1; seq <= 4; seq++) {
        unsigned char pkt[64];
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 8;
        pkt[4] = (PING_ID >> 8) & 0xFF;
        pkt[5] = PING_ID & 0xFF;
        pkt[6] = (seq >> 8) & 0xFF;
        pkt[7] = seq & 0xFF;
        for (int i = 8; i < 64; i++) pkt[i] = (unsigned char)i;

        unsigned short c = csum16(pkt, 64);
        pkt[2] = (c >> 8) & 0xFF;
        pkt[3] = c & 0xFF;

        int sent = 0;
        for (int t = 0; t < 10; t++) {
            int r = sendto(s, pkt, 64, (struct sockaddr*)&dst, sizeof(dst));
            if (r > 0) { sent = 1; break; }
            sleep_ms(50);
        }
        if (!sent) { printf("seq=%u: send failed\n", seq); continue; }

        unsigned char buf[128];
        struct sockaddr_in src;
        unsigned int srclen = sizeof(src);
        int got = 0;
        for (int t = 0; t < 20; t++) {
            int r = recvfrom(s, buf, sizeof(buf),
                             (struct sockaddr*)&src, &srclen);
            if (r >= 0) {
                printf("64 bytes from %s: seq=%u\n",
                       inet_ntoa(src.sin_addr), seq);
                got = 1;
                break;
            }
            sleep_ms(50);
        }
        if (!got) printf("seq=%u: timeout\n", seq);
        sleep_ms(200);
    }

    sockclose(s);
    return 0;
}