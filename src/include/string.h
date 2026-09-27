#ifndef STRING_H
#define STRING_H

#include <stdint.h>
#include <stddef.h>

void *memset(void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
void *memcpy(void *dest, const void *src, size_t n);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);

#endif