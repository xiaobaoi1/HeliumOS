// user/libc/getcwd.c （新建）
#include <unistd.h>
#include "syscall.h"
char *getcwd(char *buf, size_t size) {
    if (fs_getcwd(buf, (int)size) < 0) return NULL;
    return buf;
}