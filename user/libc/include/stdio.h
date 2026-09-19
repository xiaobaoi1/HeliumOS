#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/* 单字符输出 */
int putchar(int c);

/* 格式化输出到 stdout */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);

/* 格式化输出到字符串 */
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* 格式化输出到指定 fd */
int dprintf(int fd, const char *fmt, ...);

#endif