/* user/argtest.c
 * 打印自己的 argc/argv，用于验证 spawn 传参。
 */
#include "syscall.h"
#include "stdio.h"

int _start(int argc, char **argv) {
    printf("argtest: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) {
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    }
    return argc;
}