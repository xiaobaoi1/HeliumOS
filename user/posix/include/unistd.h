#ifndef POSIX_UNISTD_H
#define POSIX_UNISTD_H
#include <stddef.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* 底层实现，由 posix_io.c 提供 */
long posix_read(int fd, void *buf, size_t count);
long posix_write(int fd, const void *buf, size_t count);

/* 应用层使用宏替换，避免与 syscall.h 的原生 static inline 冲突 */
#define read  posix_read
#define write posix_write

long lseek(int fd, long offset, int whence);
int  ftruncate(int fd, long length);
int  isatty(int fd);

#endif