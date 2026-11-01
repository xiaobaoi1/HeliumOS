#include "stdio.h"
#include "stdint.h"
#include "syscall.h"

int main(void) {
    long long a = 0x123456789ABCLL;
    unsigned long long b = 0xDEADBEEFCAFEBABEULL;
    int neg = -2147483647 - 1;
    unsigned int sz = 0xFFFFFFFF;

    printf("ll:  %lld\n", a);
    printf("llu: %llu\n", b);
    printf("llx: %llx\n", b);
    printf("int: %d\n", neg);
    printf("zu:  %zu\n", (size_t)sz);
    printf("zx:  %zx\n", (size_t)sz);
    printf("mix: %d %lld %s\n", 42, a, "end");
    return 0;
}