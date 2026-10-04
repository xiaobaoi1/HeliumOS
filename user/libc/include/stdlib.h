#ifndef USER_STDLIB_H
#define USER_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t new_size);

void  exit(int status);
void  abort(void);

/* 排序 */
void  qsort(void *base, size_t n, size_t size,
            int (*cmp)(const void *, const void *));

/* 环境变量 */
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

/* libc 内部，由 crt0 调用 */
void  __libc_init_environ(char **envp);

/* 转换 */
int   atoi(const char *s);
long  atol(const char *s);
long  strtol(const char *s, char **endptr, int base);

/* 绝对值 */
int   abs(int x);
long  labs(long x);

/* 随机数 */
void  srand(unsigned int seed);
int   rand(void);

#endif