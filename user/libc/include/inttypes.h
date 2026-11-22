#ifndef USER_INTTYPES_H
#define USER_INTTYPES_H

#include <stdint.h>

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

#define PRId8   "d"
#define PRIi8   "i"
#define PRIu8   "u"
#define PRIx8   "x"
#define PRIX8   "X"

#define SCNd8   "hhd"
#define SCNi8   "hhi"
#define SCNu8   "hhu"
#define SCNx8   "hhx"
#define SCNd16  "hd"
#define SCNi16  "hi"
#define SCNu16  "hu"
#define SCNx16  "hx"
#define SCNd32  "d"
#define SCNi32  "i"
#define SCNu32  "u"
#define SCNx32  "x"
#define SCNd64  "lld"
#define SCNi64  "lli"
#define SCNu64  "llu"
#define SCNx64  "llx"

#define PRId16  "d"
#define PRIi16  "i"
#define PRIu16  "u"
#define PRIx16  "x"
#define PRIX16  "X"

#define PRId32  "d"
#define PRIi32  "i"
#define PRIu32  "u"
#define PRIx32  "x"
#define PRIX32  "X"

#define PRId64  "lld"
#define PRIi64  "lli"
#define PRIu64  "llu"
#define PRIx64  "llx"
#define PRIX64  "llX"

#define PRIdPTR "d"
#define PRIuPTR "u"
#define PRIxPTR "x"

#endif