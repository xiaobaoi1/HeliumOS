/* nc — UDP 简易版
 * 用法：
 *   nc -u <host> <port>      读 stdin，发送到 host:port，等回复
 *   nc -u -l <port>          监听模式：bind 端口，打印收到的每个包
 */
#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static uint32_t resolve(const char *host) {
    uint32_t ip = inet_addr(host);
    if (ip != 0) return ip;
    struct hostent *h = gethostbyname(host);
    if (!h) return 0;
    return *(uint32_t*)h->h_addr_list[0];
}

/* 发送模式：读 stdin 全部内容，一次发出，等回复。 */
static int do_send(const char *host, int port) {
    uint32_t ip = resolve(host);
    if (ip == 0) { printf("nc: cannot resolve %s\n", host); return 1; }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { printf("nc: socket %d\n", s); return 1; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(43211);
    bind(s, (struct sockaddr*)&local, sizeof(local));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)port);
    dst.sin_addr = ip;

    /* 从 stdin 读一行（或 EOF 前的全部） */
    char buf[1024];
    int n = read(0, buf, sizeof(buf));
    if (n <= 0) { printf("nc: no input\n"); sockclose(s); return 1; }

    /* ARP miss 时重试 */
    int sent = 0;
    for (int t = 0; t < 20; t++) {
        int r = sendto(s, buf, n, (struct sockaddr*)&dst, sizeof(dst));
        if (r > 0) { sent = 1; break; }
        sleep_ms(50);
    }
    if (!sent) { printf("nc: send failed\n"); sockclose(s); return 1; }

    /* 等回复 */
    unsigned char resp[1024];
    struct sockaddr_in src;
    for (int t = 0; t < 40; t++) {
        unsigned int srclen = sizeof(src);
        int r = recvfrom(s, resp, sizeof(resp),
                         (struct sockaddr*)&src, &srclen);
        if (r > 0) {
            printf("reply from %s:%u (%d bytes)\n",
                   inet_ntoa(src.sin_addr), ntohs(src.sin_port), r);
            write(1, resp, r);
            if (resp[r-1] != '\n') putchar('\n');
            sockclose(s);
            return 0;
        }
        sleep_ms(50);
    }
    printf("nc: timeout\n");
    sockclose(s);
    return 1;
}

/* 监听模式：绑定端口，打印每个包的来源和内容。Ctrl+C 退出。 */
static int do_listen(int port) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { printf("nc: socket %d\n", s); return 1; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons((uint16_t)port);
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        printf("nc: bind %d failed\n", port);
        sockclose(s);
        return 1;
    }

    printf("listening on udp/%d (Ctrl+C to stop)\n", port);

    for (;;) {
        unsigned char buf[1024];
        struct sockaddr_in src;
        unsigned int srclen = sizeof(src);
        int r = recvfrom(s, buf, sizeof(buf),
                         (struct sockaddr*)&src, &srclen);
        if (r > 0) {
            printf("[%s:%u] ", inet_ntoa(src.sin_addr), ntohs(src.sin_port));
            write(1, buf, r);
            if (r > 0 && buf[r-1] != '\n') putchar('\n');
        }
        sleep_ms(20);
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || strcmp(argv[1], "-u") != 0) {
        printf("usage: nc -u <host> <port>\n");
        printf("       nc -u -l <port>\n");
        return 1;
    }

    if (strcmp(argv[2], "-l") == 0) {
        if (argc < 4) { printf("usage: nc -u -l <port>\n"); return 1; }
        return do_listen(atoi(argv[3]));
    }

    if (argc < 4) { printf("usage: nc -u <host> <port>\n"); return 1; }
    return do_send(argv[2], atoi(argv[3]));
}