#ifndef ERRNO_H
#define ERRNO_H

/* 统一错误码（负数） */
#define OK           0    /* 成功 */

#define EPERM       -1    /* 操作不允许 */
#define ENOENT      -2    /* 文件/目录不存在 */
#define EIO         -3    /* I/O 错误 */
#define ENOMEM      -4    /* 内存不足 */
#define EFAULT      -5    /* 用户指针非法 */
#define EINVAL      -6    /* 参数无效 */
#define EBUSY       -7    /* 资源忙 */
#define ENOSPC      -8    /* 空间不足 */
#define ENOSYS      -9    /* 系统调用未实现 */
#define EAGAIN     -10    /* 重试 */
#define EINTR      -11    /* 被中断 */
#define ERANGE     -12    /* 超出范围 */
#define ENOTDIR    -13    /* 不是目录 */
#define EISDIR     -14    /* 是目录 */
#define EEXIST     -15    /* 已存在 */
#define ENOTEMPTY  -16    /* 目录非空 */
#define EPIPE      -17    /* 管道破裂 */
#define ECHILD     -18    /* 没有子进程 */
#define ENOEXEC    -19    /* 可执行格式错误 */
#define ENAMETOOLONG -20    /* 文件名过长 */
#define EXDEV      -21    /* 跨设备 */
#define EADDRINUSE -22    /* 端口已被占用 */

#endif