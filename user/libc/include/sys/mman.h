#ifndef USER_SYS_MMAN_H
#define USER_SYS_MMAN_H

#include <stddef.h>

#define PROT_NONE     0x00
#define PROT_READ     0x01
#define PROT_WRITE    0x02
#define PROT_EXEC     0x04

/* 加上 #ifndef 保护，如果 syscall.h 里已经定义了，就优先用 syscall.h 里的值 */
#ifndef MAP_SHARED
#define MAP_SHARED    0x02
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE   0x01
#endif
#ifndef MAP_FIXED
#define MAP_FIXED     0x10
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x04
#endif
#ifndef MAP_FILE
#define MAP_FILE      0x08
#endif

#define MAP_FAILED    ((void *)-1)

void *mmap(void *addr, size_t length, int prot, int flags, int fd, long offset);
int   munmap(void *addr, size_t length);

#endif