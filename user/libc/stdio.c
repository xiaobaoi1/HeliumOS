#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdarg.h>

#define FILE_BUF_SIZE 512

struct __helio_file {
    int type;
    int fd;
    int eof;
    int err;
    unsigned int pos;
    unsigned int buf_len;
    char buf[FILE_BUF_SIZE];
};

FILE _stdin = {1, -1, 0, 0, 0, 0, {0}};
FILE _stdout = {2, -1, 0, 0, 0, 0, {0}};
FILE _stderr = {3, -1, 0, 0, 0, 0, {0}};

FILE *stdin = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

FILE *fopen(const char *path, const char *mode) {
    int flags = FS_O_RDONLY;
    if (mode[0] == 'w') flags = FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC;
    else if (mode[0] == 'a') flags = FS_O_WRONLY | FS_O_CREAT;
    
    int fd = fs_open(path, flags);
    if (fd < 0) return NULL;

    FILE *f = (FILE*)malloc(sizeof(FILE));
    if (!f) { fs_close(fd); return NULL; }
    
    f->type = 0;
    f->fd = fd;
    f->eof = 0;
    f->err = 0;
    f->pos = 0;
    f->buf_len = 0;
    return f;
}

int fflush(FILE *stream) {
    if (!stream) return 0;
    if (stream->type != 0) return 0;
    if (stream->buf_len > 0) {
        int w = fs_write(stream->fd, stream->buf, stream->buf_len);
        if (w != (int)stream->buf_len) stream->err = 1;
        stream->buf_len = 0;
    }
    return stream->err ? -1 : 0;
}

int fclose(FILE *stream) {
    if (!stream) return -1;
    if (stream == stdin || stream == stdout || stream == stderr) {
        fflush(stream);
        return 0;
    }
    fflush(stream);
    fs_close(stream->fd);
    free(stream);
    return 0;
}

unsigned int fread(void *ptr, unsigned int size, unsigned int nmemb, FILE *stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;
    unsigned int total = size * nmemb;
    
    if (stream->type == 1) {
        int r = read(0, ptr, total);
        if (r <= 0) { stream->eof = 1; return 0; }
        return r / size;
    }

    unsigned int done = 0;
    char *dst = (char*)ptr;
    
    while (done < total && stream->pos < stream->buf_len) {
        dst[done++] = stream->buf[stream->pos++];
    }
    
    if (done == total) return nmemb;
    
    if (stream->pos == stream->buf_len && total - done >= FILE_BUF_SIZE) {
        int r = fs_read(stream->fd, dst + done, total - done);
        if (r <= 0) { stream->eof = 1; return done / size; }
        return (done + r) / size;
    }
    
    stream->pos = 0;
    int r = fs_read(stream->fd, stream->buf, FILE_BUF_SIZE);
    if (r <= 0) { stream->eof = 1; return done / size; }
    stream->buf_len = r;
    
    while (done < total && stream->pos < stream->buf_len) {
        dst[done++] = stream->buf[stream->pos++];
    }
    return done / size;
}

unsigned int fwrite(const void *ptr, unsigned int size, unsigned int nmemb, FILE *stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;
    unsigned int total = size * nmemb;
    
    if (stream->type == 2 || stream->type == 3) {
        int fnum = (stream->type == 2) ? 1 : 2;
        int w = write(fnum, ptr, total);
        return w > 0 ? w / size : 0;
    }
    
    unsigned int done = 0;
    const char *src = (const char*)ptr;
    
    while (done < total) {
        if (stream->buf_len == FILE_BUF_SIZE) fflush(stream);
        stream->buf[stream->buf_len++] = src[done++];
    }
    
    return done / size;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    
    if (len > 0) {
        unsigned int to_write = len;
        if (to_write > sizeof(buf) - 1) to_write = sizeof(buf) - 1;
        fwrite(buf, 1, to_write, stream);
    }
    return len;
}

int fgetc(FILE *stream) {
    char c;
    if (fread(&c, 1, 1, stream) == 1) return (unsigned char)c;
    return -1;
}

int fputc(int c, FILE *stream) {
    char ch = (char)c;
    fwrite(&ch, 1, 1, stream);
    return (unsigned char)ch;
}