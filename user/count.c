// user/count.c
#include "syscall.h"
#include "stdio.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char buf[256];
    int total = 0;
    int n;
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        total += n;
    }
    printf("count: %d bytes\n", total);
    return 0;
}