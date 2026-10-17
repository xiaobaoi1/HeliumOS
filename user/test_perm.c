#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* 用户程序不是 WM——fb_claim 应 EPERM */
    int r = fb_claim();
    printf("fb_claim: %d (expect -1 = EPERM)\n", r);

    struct fb_user_info info;
    r = fb_map(&info);
    printf("fb_map: %d (expect -1 = EPERM)\n", r);

    r = input_claim();
    printf("input_claim: %d (expect -1 = EPERM)\n", r);

    return 0;
}