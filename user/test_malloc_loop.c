#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

int main(void) {
    void *ptrs[64];
    for (int i = 0; i < 64; i++) {
        ptrs[i] = malloc(48);
        if (!ptrs[i]) {
            printf("malloc %d FAIL\n", i);
            return 1;
        }
        memset(ptrs[i], 0xAB, 48);
    }
    printf("malloc x64 OK\n");

    /* realloc 链 */
    char *p = NULL;
    for (int i = 1; i <= 50; i++) {
        p = realloc(p, i * 32);
        if (!p) {
            printf("realloc %d FAIL\n", i);
            return 1;
        }
        memset(p, 'x', i * 32);
    }
    free(p);
    printf("realloc x50 OK\n");

    for (int i = 0; i < 64; i++) free(ptrs[i]);
    printf("all OK\n");
    return 0;
}