#include "stdio.h"
#include "syscall.h"
#include "stdlib.h"
#include "string.h"
#include <stdarg.h>

#define FILE_BUF_SIZE 512

struct __helio_file {
    int type;       /* 0 = file, 1 = stdin, 2 = stdout, 3 = stderr */
    int fd;
    int eof;
    int err;
    unsigned int offset;    /* 当前文件偏移 */
    unsigned int pos;
    unsigned int buf_len;
    char buf[FILE_BUF_SIZE];
};

FILE _stdin = {1, -1, 0, 0, 0, 0, 0, {0}};
FILE _stdout = {2, -1, 0, 0, 0, 0, 0, {0}};
FILE _stderr = {3, -1, 0, 0, 0, 0, 0, {0}};

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
    f->offset = 0;
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
        else stream->offset += stream->buf_len;
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
        stream->offset += r;
        return r / size;
    }

    unsigned int done = 0;
    char *dst = (char*)ptr;
    
    while (done < total && stream->pos < stream->buf_len) {
        dst[done++] = stream->buf[stream->pos++];
    }
    
    if (done == total) {
        stream->offset += done;
        return nmemb;
    }
    
    if (stream->pos == stream->buf_len && total - done >= FILE_BUF_SIZE) {
        int r = fs_read(stream->fd, dst + done, total - done);
        if (r <= 0) { stream->eof = 1; return done / size; }
        stream->offset += (done + r);
        return (done + r) / size;
    }
    
    stream->pos = 0;
    int r = fs_read(stream->fd, stream->buf, FILE_BUF_SIZE);
    if (r <= 0) { stream->eof = 1; return done / size; }
    stream->buf_len = r;
    
    while (done < total && stream->pos < stream->buf_len) {
        dst[done++] = stream->buf[stream->pos++];
    }
    stream->offset += done;
    return done / size;
}

unsigned int fwrite(const void *ptr, unsigned int size, unsigned int nmemb, FILE *stream) {
    if (!stream || size == 0 || nmemb == 0) return 0;
    unsigned int total = size * nmemb;
    
    if (stream->type == 2 || stream->type == 3) {
        int fnum = (stream->type == 2) ? 1 : 2;
        int w = write(fnum, ptr, total);
        if (w > 0) stream->offset += w;
        return w > 0 ? w / size : 0;
    }
    
    unsigned int done = 0;
    const char *src = (const char*)ptr;
    
    while (done < total) {
        if (stream->buf_len == FILE_BUF_SIZE) fflush(stream);
        stream->buf[stream->buf_len++] = src[done++];
    }
    
    stream->offset += done;
    return done / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream) return -1;
    
    long new_off = offset;
    if (whence == 0) { /* SEEK_SET */ }
    else if (whence == 1) { /* SEEK_CUR */ new_off = stream->offset + offset; }
    else if (whence == 2) { /* SEEK_END */
        struct fstat_buf st;
        if (fs_fstat(stream->fd, &st) < 0) return -1;
        new_off = st.size + offset;
    } else {
        return -1;
    }
    
    if (new_off < 0) return -1;
    
    if (stream->type == 0) {
        if (fs_seek(stream->fd, new_off) < 0) return -1;
    }
    
    stream->offset = new_off;
    stream->pos = 0;
    stream->buf_len = 0;
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    return stream ? (long)stream->offset : -1;
}

int fputs(const char *s, FILE *stream) {
    if (!s) return -1;
    return fwrite(s, 1, strlen(s), stream);
}

int puts(const char *s) {
    if (!s) return -1;
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

int vfprintf(FILE *stream, const char *format, va_list ap) {
    char buf[1024];
    int len = vsnprintf(buf, sizeof(buf), format, ap);
    if (len > 0) fwrite(buf, 1, len, stream);
    return len;
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

int sscanf(const char *str, const char *format, ...) {
    if (!str || !format) return 0;
    if (format[0] == '%' && format[1] == 'd') {
        va_list ap;
        va_start(ap, format);
        int *a = va_arg(ap, int*);
        int *b = va_arg(ap, int*);
        va_end(ap);
        
        int v = 0;
        while (*str >= '0' && *str <= '9') { v = v * 10 + (*str - '0'); str++; }
        if (*str == ';') {
            str++;
            int v2 = 0;
            while (*str >= '0' && *str <= '9') { v2 = v2 * 10 + (*str - '0'); str++; }
            if (a) *a = v;
            if (b) *b = v2;
            return 2;
        }
    }
    return 0;
}

void perror(const char *s) {
    if (s) dprintf(2, "%s: error\n", s);
    else   dprintf(2, "error\n");
}

FILE *__stdio_create_from_handle(int handle, unsigned int offset) {
    FILE *f = (FILE*)malloc(sizeof(FILE));
    if (!f) return NULL;
    
    f->type = 0;
    f->fd = handle;      /* 直接存内核的 fs_handle */
    f->eof = 0;
    f->err = 0;
    f->offset = offset;
    f->pos = 0;
    f->buf_len = 0;
    return f;
}