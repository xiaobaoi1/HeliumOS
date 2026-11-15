#include <syscall.h>
#include <stdlib.h>
#include <string.h>

// void *shim_mmap(void *addr, size_t length, int prot, int flags, int fd, long offset) {
//     (void)addr; (void)prot; (void)flags; (void)offset;
//     void *buf = malloc(length);
//     if (buf && fd >= 0) {
//         read(fd, buf, length);
//     }
//     return buf;
// }

// int shim_munmap(void *addr, size_t length) {
//     (void)length;
//     free(addr);
//     return 0;
// }