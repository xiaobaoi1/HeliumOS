#ifndef USER_STDLIB_H
#define USER_STDLIB_H

#include <stddef.h>

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t n, size_t size);
void *realloc(void *ptr, size_t new_size);

void  exit(int status);

#endif