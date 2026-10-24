#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct __helio_file FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
unsigned int fread(void *ptr, unsigned int size, unsigned int nmemb, FILE *stream);
unsigned int fwrite(const void *ptr, unsigned int size, unsigned int nmemb, FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int fflush(FILE *stream);
int fgetc(FILE *stream);
int fputc(int c, FILE *stream);

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