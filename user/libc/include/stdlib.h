#ifndef USER_STDLIB_H
#define USER_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t new_size);

void  exit(int status);

/* 环境变量 */
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

/* libc 内部，由 crt0 调用 */
void  __libc_init_environ(char **envp);

#endif