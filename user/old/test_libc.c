#include "libc/include/stdio.h"
#include "libc/include/stdlib.h"
#include "libc/include/string.h"
#include "libc/include/syscall.h"

void _start(void) {
    printf("\n=== HeliumOS libc test ===\n");

    /* string */
    char a[32] = "hello";
    char b[32];
    strcpy(b, a);
    printf("strcpy: %s\n", b);
    printf("strlen: %d\n", (int)strlen(a));
    printf("strcmp: %d\n", strcmp(a, b));

    /* printf 全格式 */
    printf("dec: %d\n", -12345);
    printf("hex: %x\n", 0xDEADBEEF);
    printf("HEX: %X\n", 0xCAFE);
    printf("unsigned: %u\n", 4000000000u);
    printf("char: %c%c%c\n", 'X', 'Y', 'Z');
    printf("ptr: %p\n", (void*)0x12345678);
    printf("percent: 100%%\n");

    /* sprintf */
    char buf[64];
    snprintf(buf, sizeof(buf), "n=%d s=%s", 42, "done");
    printf("snprintf: %s\n", buf);

    /* malloc */
    int *arr = (int*)malloc(10 * sizeof(int));
    if (!arr) {
        printf("malloc failed\n");
        _exit(1);
    }
    for (int i = 0; i < 10; i++) arr[i] = i * i;
    printf("malloc array: ");
    for (int i = 0; i < 10; i++) printf("%d ", arr[i]);
    printf("\n");

    /* realloc */
    arr = (int*)realloc(arr, 20 * sizeof(int));
    for (int i = 10; i < 20; i++) arr[i] = i * i;
    printf("after realloc: arr[15]=%d\n", arr[15]);
    free(arr);

    /* calloc */
    char *z = (char*)calloc(16, 1);
    int all_zero = 1;
    for (int i = 0; i < 16; i++) if (z[i]) all_zero = 0;
    printf("calloc zeros: %s\n", all_zero ? "yes" : "no");
    free(z);

    printf("=== all done ===\n");
    _exit(0);
}