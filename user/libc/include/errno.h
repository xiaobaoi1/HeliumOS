#ifndef USER_ERRNO_H
#define USER_ERRNO_H

/* 与内核 src/include/errno.h 保持一致 */
#define OK           0
#define EPERM       -1
#define ENOENT      -2
#define EIO         -3
#define ENOMEM      -4
#define EFAULT      -5
#define EINVAL      -6
#define EBUSY       -7
#define ENOSPC      -8
#define ENOSYS      -9
#define EAGAIN     -10
#define EINTR      -11
#define ERANGE     -12
#define ENOTDIR    -13
#define EISDIR     -14
#define EEXIST     -15
#define ENOTEMPTY  -16
#define EPIPE      -17
#define ECHILD     -18
#define ENOEXEC    -19
#define ENAMETOOLONG -20
#define EXDEV      -21
#define EADDRINUSE -22
#define ETIMEDOUT  -23
#define ECONNREFUSED -24

#endif