#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== UDP test ===\n");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    printf("socket: %d\n", s);
    if (s < 0) return 1;

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(1234);

    int r = bind(s, (struct sockaddr*)&local, sizeof(local));
    printf("bind: %d\n", r);
    if (r < 0) { sockclose(s); return 1; }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5555);
    dst.sin_addr = inet_addr("10.0.2.2");

    const char *msg = "hello udp";
    for (int i = 0; i < 10; i++) {
        r = sendto(s, msg, strlen(msg), (struct sockaddr*)&dst, sizeof(dst));
        if (r >= 0) break;
        sleep_ms(100);
    }
    printf("sendto: %d (expect %d)\n", r, (int)strlen(msg));

    char buf[128];
    struct sockaddr_in src;
    unsigned int srclen = sizeof(src);
    r = recvfrom(s, buf, sizeof(buf), (struct sockaddr*)&src, &srclen);
    printf("recvfrom: %d (expect -10 = EAGAIN)\n", r);

    sockclose(s);
    printf("=== done ===\n");
    return 0;
}