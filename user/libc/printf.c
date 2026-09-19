#include "stdio.h"
#include "syscall.h"     /* ← 新增，为了 write */

/* ---------- putchar ---------- */

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return (unsigned char)ch;
}

/* ---------- 输出后端 ---------- */

typedef void (*putc_fn)(char c, void *ctx);

static void putc_fd(char c, void *ctx) {
    int fd = (int)(long)ctx;
    write(fd, &c, 1);
}

struct str_ctx {
    char  *buf;
    size_t pos;
    size_t limit;
};

static void putc_str(char c, void *ctx) {
    struct str_ctx *sc = (struct str_ctx*)ctx;
    if (sc->pos + 1 < sc->limit) {
        sc->buf[sc->pos] = c;
    }
    sc->pos++;
}

/* ---------- 数字输出 ---------- */

static void print_uint(putc_fn putc, void *ctx, unsigned int v,
                       int base, int uppercase) {
    char buf[32];
    int i = 0;
    if (v == 0) { putc('0', ctx); return; }
    while (v > 0) {
        unsigned int d = v % base;
        buf[i++] = (d < 10) ? ('0' + d)
                            : ((uppercase ? 'A' : 'a') + (d - 10));
        v /= base;
    }
    while (i > 0) putc(buf[--i], ctx);
}

static void print_int(putc_fn putc, void *ctx, int v) {
    if (v < 0) {
        putc('-', ctx);
        print_uint(putc, ctx, (unsigned int)(-(long)v), 10, 0);
    } else {
        print_uint(putc, ctx, (unsigned int)v, 10, 0);
    }
}

static void print_hex8(putc_fn putc, void *ctx, unsigned int v) {
    putc('0', ctx);
    putc('x', ctx);
    for (int i = 7; i >= 0; i--) {
        unsigned int d = (v >> (i * 4)) & 0xF;
        putc((d < 10) ? ('0' + d) : ('a' + d - 10), ctx);
    }
}

/* ---------- 核心格式化 ---------- */

static void vformat(putc_fn putc, void *ctx, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++, ctx);
            continue;
        }
        fmt++;
        if (*fmt == '%') { putc('%', ctx); fmt++; continue; }

        switch (*fmt) {
            case 'd':
            case 'i':
                print_int(putc, ctx, va_arg(ap, int));
                break;
            case 'u':
                print_uint(putc, ctx, va_arg(ap, unsigned int), 10, 0);
                break;
            case 'x':
                print_uint(putc, ctx, va_arg(ap, unsigned int), 16, 0);
                break;
            case 'X':
                print_uint(putc, ctx, va_arg(ap, unsigned int), 16, 1);
                break;
            case 'p':
                print_hex8(putc, ctx, (unsigned int)(long)va_arg(ap, void*));
                break;
            case 'c':
                putc((char)va_arg(ap, int), ctx);
                break;
            case 's': {
                const char *s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                while (*s) putc(*s++, ctx);
                break;
            }
            case 'l':
                fmt++;
                if (*fmt == 'd' || *fmt == 'i') {
                    print_int(putc, ctx, va_arg(ap, long));
                } else if (*fmt == 'u') {
                    print_uint(putc, ctx, va_arg(ap, unsigned long), 10, 0);
                } else if (*fmt == 'x') {
                    print_uint(putc, ctx, va_arg(ap, unsigned long), 16, 0);
                }
                break;
            default:
                putc('%', ctx);
                putc(*fmt, ctx);
                break;
        }
        fmt++;
    }
}

/* ---------- 对外接口 ---------- */

int vprintf(const char *fmt, va_list ap) {
    vformat(putc_fd, (void*)1, fmt, ap);
    return 0;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vformat(putc_fd, (void*)1, fmt, ap);
    va_end(ap);
    return 0;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vformat(putc_fd, (void*)(long)fd, fmt, ap);
    va_end(ap);
    return 0;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct str_ctx sc = { buf, 0, size };
    vformat(putc_str, &sc, fmt, ap);
    size_t end = (sc.pos < size) ? sc.pos : (size ? size - 1 : 0);
    if (size > 0) buf[end] = '\0';
    return (int)sc.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return r;
}