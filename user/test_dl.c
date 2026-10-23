#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "dlfcn.h"

int main(void) {
    dprintf(2, "[dl] test 1: basic lib\n");
    void *h = dlopen("SYS:/LIBMATH.SO", RTLD_NOW);
    if (!h) { dprintf(2, "[dl] dlopen failed: %s\n", dlerror()); return 1; }

    int (*add)(int, int) = dlsym(h, "add");
    int (*bump)(void)    = dlsym(h, "bump");
    if (add) dprintf(2, "[dl] add(2,3) = %d\n", add(2, 3));
    if (bump) {
        dprintf(2, "[dl] bump() = %d\n", bump());
        dprintf(2, "[dl] bump() = %d\n", bump());
    }
    dlclose(h);

    dprintf(2, "[dl] test 2: dependency chain\n");
    void *h2 = dlopen("SYS:/LIBMYLIB.SO", RTLD_NOW);
    if (!h2) { dprintf(2, "[dl] dlopen libmylib failed: %s\n", dlerror()); return 1; }

    int (*add_twice)(int, int) = dlsym(h2, "add_twice");
    if (!add_twice) {
        dprintf(2, "[dl] dlsym add_twice failed: %s\n", dlerror());
        return 1;
    }
    
    dprintf(2, "[dl] add_twice pointer = 0x%x\n", (unsigned int)add_twice);
    dprintf(2, "[dl] add_twice(2,3) = %d\n", add_twice(2, 3));

    dlclose(h2);
    dprintf(2, "[dl] done\n");
    return 0;
}