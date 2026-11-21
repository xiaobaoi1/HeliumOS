#include <unistd.h>
#include "syscall.h"

long pread(int fd, void *buf, size_t count, unsigned int offset) {
    if (fs_seek(fd, offset) < 0) return -1;
    return fs_read(fd, buf, count);
}

long pwrite(int fd, const void *buf, size_t count, unsigned int offset) {
    if (fs_seek(fd, offset) < 0) return -1;
    return fs_write(fd, buf, count);
}