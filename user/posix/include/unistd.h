#ifndef POSIX_UNISTD_H
#define POSIX_UNISTD_H

#include <stddef.h>

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* read 和 write 会和 syscall.h 里的原生包装冲突，因此重定向 */
long posix_read(int fd, void *buf, size_t count);
long posix_write(int fd, const void *buf, size_t count);

#define read  posix_read
#define write posix_write

/* 以下函数原生库中没有，无需重定向，直接声明标准接口 */
long lseek(int fd, long offset, int whence);
int  ftruncate(int fd, long length);
int  isatty(int fd);

#endif