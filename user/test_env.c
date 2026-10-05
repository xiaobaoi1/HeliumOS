#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include "stdio.h"
#include "string.h"

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    printf("socket: %d\n", s);
    if (s < 0) return 1;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(80);
    dst.sin_addr = inet_addr("10.0.2.2");

    int r = connect(s, (struct sockaddr*)&dst, sizeof(dst));
    printf("connect: %d (expect -9 = ENOSYS)\n", r);

    r = listen(s, 8);
    printf("listen: %d (expect -9)\n", r);

    sockclose(s);
    return 0;
}