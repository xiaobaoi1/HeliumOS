#ifndef HELIUM_7Z_COMPAT_H
#define HELIUM_7Z_COMPAT_H
#include <stdio.h>
#include <errno.h>
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif
#ifndef EOF
#define EOF (-1)
#endif
#define errno 0
static inline int ferror(FILE *f) { (void)f; return 0; }
#endif
