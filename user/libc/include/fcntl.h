#ifndef USER_FCNTL_H
#define USER_FCNTL_H

#define O_RDONLY  0x01
#define O_WRONLY  0x02
#define O_RDWR    0x03
#define O_CREAT   0x04
#define O_TRUNC   0x08
#define O_APPEND  0x10

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200
#endif

int open(const char *pathname, int flags, ...);
int close(int fd);

#endif