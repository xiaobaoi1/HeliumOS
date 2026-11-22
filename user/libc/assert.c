#include "assert.h"
#include "stdio.h"
#include "stdlib.h"

void __assert_fail(const char *expr, const char *file,
                   unsigned int line, const char *func) {
    dprintf(2, "%s:%u: %s: assertion '%s' failed\n",
            file, line, func, expr);
    abort();
}