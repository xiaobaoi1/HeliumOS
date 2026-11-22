#include "execinfo.h"

int backtrace(void **buffer, int size) {
    (void)buffer; (void)size;
    return 0;
}

void backtrace_symbols_fd(void *const *buffer, int size, int fd) {
    (void)buffer; (void)size; (void)fd;
}

char **backtrace_symbols(void *const *buffer, int size) {
    (void)buffer; (void)size;
    return 0;
}