#ifndef USER_DIRENT_H
#define USER_DIRENT_H

#include "syscall.h"

/* POSIX 字段名映射：d_name → name */
#ifndef d_name
#define d_name name
#endif

typedef struct __user_DIR DIR;

DIR  *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int   closedir(DIR *dirp);
int   dirfd(DIR *dirp);
int scandir(const char *dirp, struct dirent ***namelist,
            int (*filter)(const struct dirent *),
            int (*compar)(const struct dirent **, const struct dirent **));
#endif