#ifndef USER_SYS_STAT_H
#define USER_SYS_STAT_H

#include <stdint.h>
#include <time.h>

struct stat {
    uint32_t st_size;
    uint32_t st_mode;
    time_t st_mtime;
};

#define S_IFMT  0170000
#define S_IFDIR 0040000
#define S_IFREG 0100000

#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRWXO 0007

#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR        0x200
#endif

int fstatat(int dirfd, const char *path, struct stat *buf, int flags);
int stat(const char *path, struct stat *buf);
int mkdir(const char *path, unsigned int mode);

#endif