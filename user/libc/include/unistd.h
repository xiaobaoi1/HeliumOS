#ifndef USER_UNISTD_H
#define USER_UNISTD_H

#include <stddef.h>

#ifndef _OFF_T_DEFINED
#define _OFF_T_DEFINED
typedef long off_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef int ssize_t;
#endif

#ifndef _MODE_T_DEFINED
#define _MODE_T_DEFINED
typedef unsigned int mode_t;
#endif

#ifndef _PID_T_DEFINED
#define _PID_T_DEFINED
typedef int pid_t;
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define R_OK 4
#define W_OK 2
#define X_OK 1
#define F_OK 0

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200
#endif

/* ---------- 基础 I/O ---------- */
ssize_t read(int fd, void *buf, size_t count);
ssize_t write(int fd, const void *buf, size_t count);
int     close(int fd);
int     isatty(int fd);

/* ---------- 文件系统 ---------- */
int     access(const char *pathname, int mode);
off_t   lseek(int fd, off_t offset, int whence);
int     ftruncate(int fd, off_t length);
int     rmdir(const char *path);
int     unlink(const char *pathname);
int     unlinkat(int dirfd, const char *pathname, int flags);
int     chdir(const char *path);
char   *getcwd(char *buf, size_t size);

long    pread(int fd, void *buf, size_t count, unsigned int offset);
long    pwrite(int fd, const void *buf, size_t count, unsigned int offset);

#endif