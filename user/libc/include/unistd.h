#ifndef USER_UNISTD_H
#define USER_UNISTD_H

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int lseek(int fd, int off, int whence);
int ftruncate(int fd, int len);

#endif