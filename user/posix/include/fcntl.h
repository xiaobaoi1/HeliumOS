#ifndef POSIX_FCNTL_H
#define POSIX_FCNTL_H

#include "syscall.h"

#define O_RDONLY FS_O_RDONLY
#define O_WRONLY FS_O_WRONLY
#define O_RDWR   FS_O_RDWR
#define O_CREAT  FS_O_CREAT
#define O_TRUNC  FS_O_TRUNC

int open(const char *path, int flags, ...);
int close(int fd);

#endif