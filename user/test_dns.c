#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== DNS test ===\n");

    /* 先测试点分十进制直接通过 */
    struct hostent *h = gethostbyname("10.0.2.2");
    if (h) {
        printf("gethostbyname(10.0.2.2) = %s\n",
               inet_ntoa(*(uint32_t*)h->h_addr_list[0]));
    }

    /* 解析域名。QEMU slirp 内置 DNS 转发。 */
    const char *names[] = {
        "example.com",
        "one.one.one.one",
        NULL
    };

    for (int i = 0; names[i]; i++) {
        h = gethostbyname(names[i]);
        if (h) {
            printf("%s -> %s\n",
                   names[i],
                   inet_ntoa(*(uint32_t*)h->h_addr_list[0]));
        } else {
            printf("%s -> (fail)\n", names[i]);
        }
    }

    printf("=== done ===\n");
    return 0;
}