#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "dlfcn.h"

int main(void) {
    dprintf(2, "[dl] loading SYS:/LIBMATH.SO\n");

    void *h = dlopen("SYS:/LIBMATH.SO", RTLD_NOW);
    if (!h) {
        dprintf(2, "[dl] dlopen failed: %s\n", dlerror());
        return 1;
    }

    int (*add)(int, int) = dlsym(h, "add");
    int (*mul)(int, int) = dlsym(h, "mul");
    int (*bump)(void)    = dlsym(h, "bump");

    if (!add || !mul || !bump) {
        dprintf(2, "[dl] dlsym failed: %s\n", dlerror());
        return 1;
    }

    dprintf(2, "[dl] add(2,3) = %d\n", add(2, 3));
    dprintf(2, "[dl] mul(4,5) = %d\n", mul(4, 5));
    dprintf(2, "[dl] bump() = %d\n", bump());
    dprintf(2, "[dl] bump() = %d\n", bump());

    dlclose(h);
    dprintf(2, "[dl] done\n");
    return 0;
}