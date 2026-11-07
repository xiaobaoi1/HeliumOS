#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "assert.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    assert(strcmp(strcat((char[16]){"ab"}, "cd"), "abcd") == 0);
    assert(strcmp(strstr("hello world", "world"), "world") == 0);
    assert(strstr("hello", "xyz") == NULL);
    assert(strrchr("a/b/c", '/') != NULL);
    assert(*strrchr("a/b/c", '/') == '/');

    assert(atoi("-123") == -123);
    assert(atoi("  42abc") == 42);
    assert(strtol("ff", NULL, 16) == 255);
    assert(strtol("0x1F", NULL, 0) == 31);

    assert(abs(-5) == 5);
    assert(labs(-5L) == 5L);

    srand(1);
    int a = rand();
    srand(1);
    int b = rand();
    assert(a == b);   /* 同种子同序列 */

    printf("libc 4C tests passed\n");
    return 0;
}