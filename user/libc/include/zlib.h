#ifndef USER_ZLIB_H
#define USER_ZLIB_H

#include <stdint.h>
#include <stddef.h>

#define Z_NO_FLUSH    0
#define Z_OK          0
#define Z_STREAM_END  1
#define Z_STREAM_ERROR (-2)
#define Z_DATA_ERROR   (-3)
#define Z_MEM_ERROR    (-4)
#define Z_BUF_ERROR    (-5)

#define MAX_WBITS     15
#define Z_NULL        ((void *)0)

typedef struct z_stream_s {
    uint8_t       *next_in;
    unsigned int   avail_in;
    unsigned int   total_in;
    uint8_t       *next_out;
    unsigned int   avail_out;
    unsigned int   total_out;
    const char    *msg;
    void          *state;
    void          *zalloc;
    void          *zfree;
    void          *opaque;
    int            data_type;
    unsigned long  adler;
    unsigned long  reserved;
} z_stream;

typedef struct gzFile_s *gzFile;

int inflateInit2(z_stream *strm, int windowBits);
int inflate(z_stream *strm, int flush);
int inflateEnd(z_stream *strm);

gzFile gzopen(const char *path, const char *mode);
char  *gzgets(gzFile file, char *buf, int len);
int    gzclose(gzFile file);

#endif