#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: TCPSERVER <port>\n"); return 1; }

    int port = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') { printf("bad port\n"); return 1; }
        port = port * 10 + (*p - '0');
    }
    if (port <= 0 || port > 65535) { printf("bad port\n"); return 1; }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { printf("socket: %d\n", s); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr = 0;

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("bind failed\n"); return 1;
    }
    if (listen(s, 8) < 0) { printf("listen failed\n"); return 1; }

    printf("listening on tcp/%d\n", port);

    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) { sleep_ms(20); continue; }

        printf("accepted fd=%d\n", c);

        /* 回显循环：读到啥回啥 */
        for (;;) {
            char buf[512];
            int n = recvfrom(c, buf, sizeof(buf), NULL, NULL);
            if (n > 0) {
                printf("recv %d bytes: ", n);
                write(1, buf, n);
                if (buf[n-1] != '\n') putchar('\n');

                /* 回发 */
                sendto(c, buf, n, NULL, 0);
            } else if (n < 0) {
                break;
            }
            sleep_ms(20);
        }
        printf("closed fd=%d\n", c);
        sockclose(c);
    }
}