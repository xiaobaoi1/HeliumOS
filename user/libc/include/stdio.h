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

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fputs(const char *s, FILE *stream);
int puts(const char *s);
int vfprintf(FILE *stream, const char *format, va_list ap);
FILE *fdopen(int fd, const char *mode);

int putchar(int c);
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int dprintf(int fd, const char *fmt, ...);

int sscanf(const char *str, const char *format, ...);
void perror(const char *s);


/* 供 POSIX 层内部使用的构造器 */
FILE *__stdio_create_from_handle(int handle, unsigned int offset);

#endif