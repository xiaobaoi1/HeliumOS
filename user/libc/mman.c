#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include "syscall.h"

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
    if (flags & MAP_ANONYMOUS) {
        unsigned int res = sys_mmap((unsigned int)addr, length, prot, flags, -1, offset);
        return res == 0 ? MAP_FAILED : (void *)res;
    }
    void *buf = malloc(length);
    if (!buf) return MAP_FAILED;
    if (fd >= 0) {
        if (lseek(fd, offset, SEEK_SET) >= 0) {
            if (sys_read(fd, buf, length) < 0) {
                free(buf);
                return MAP_FAILED;
            }
        }
    }
    return buf;
}

int munmap(void *addr, size_t length) {
    (void)length;
    free(addr);
    return 0;
}