#include "syscall.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== UDP test ===\n");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    printf("socket: %d\n", s);
    if (s < 0) return 1;

    int r = bind(s, htons(1234));
    printf("bind: %d\n", r);
    if (r < 0) { sockclose(s); return 1; }

    const char *msg = "hello udp";
    unsigned int dst_ip_be = htonl(0x0A000202);   /* 10.0.2.2 */
    unsigned short dst_port_be = htons(5555);

    /* 第一次 sendto 可能因 ARP miss 失败；重试 */
    for (int i = 0; i < 10; i++) {
        r = sendto(s, dst_ip_be, dst_port_be, msg, strlen(msg));
        if (r >= 0) break;
        sleep_ms(100);
    }
    printf("sendto: %d (expect %d)\n", r, (int)strlen(msg));

    char buf[128];
    unsigned int src_ip = 0;
    unsigned short src_port = 0;
    r = recvfrom(s, buf, sizeof(buf), &src_ip, &src_port);
    printf("recvfrom: %d (expect -10 = EAGAIN)\n", r);

    sockclose(s);
    printf("=== done ===\n");
    return 0;
}