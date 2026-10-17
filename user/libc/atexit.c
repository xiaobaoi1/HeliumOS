#include "stdlib.h"
#include <stddef.h>

#define ATEXIT_MAX 16

static void (*g_fns[ATEXIT_MAX])(void);
static int g_count = 0;

int atexit(void (*fn)(void)) {
    if (!fn || g_count >= ATEXIT_MAX) return -1;
    g_fns[g_count++] = fn;
    return 0;
}

/* 内部：由 exit() 调用，逆序执行 */
void __libc_run_atexit(void) {
    for (int i = g_count - 1; i >= 0; i--) {
        if (g_fns[i]) g_fns[i]();
    }
    g_count = 0;
}