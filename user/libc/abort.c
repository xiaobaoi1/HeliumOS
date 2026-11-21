#include "stdlib.h"
#include "signal.h"
#include "syscall.h"

void abort(void) {
    raise(SIGABRT);
    /* 信号被忽略或阻塞时的兜底 */
    proc_exit(128 + SIGABRT);
}