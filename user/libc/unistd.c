#include <unistd.h>
#include "syscall.h"

int lseek(int fd, int off, int whence) {
    if (whence != SEEK_SET) return -1;
    if (fs_seek(fd, off) < 0) return -1;
    return off;
}

int ftruncate(int fd, int len) {
    if (len != 0) return -1;
    return fs_truncate(fd);
}