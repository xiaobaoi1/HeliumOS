#include "stdio.h"
#include "syscall.h"
#include <stdint.h>

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

/* ---------- 无符号整数 ---------- */

static void print_uint(putc_fn putc, void *ctx, uint32_t v,
                       int base, int uppercase,
                       int width, int zero_pad, int left_align) {
    char buf[32];
    int len = 0;
    const char *digits = uppercase ? "0123456789ABCDEF"
                                   : "0123456789abcdef";

    if (v == 0) {
        buf[len++] = '0';
    } else {
        while (v > 0) {
            buf[len++] = digits[v % base];
            v /= base;
        }
    }

    int pad = (width > len) ? width - len : 0;

    if (left_align) {
        for (int i = len - 1; i >= 0; i--) putc(buf[i], ctx);
        for (int i = 0; i < pad; i++) putc(' ', ctx);
    } else {
        char pc = zero_pad ? '0' : ' ';
        for (int i = 0; i < pad; i++) putc(pc, ctx);
        for (int i = len - 1; i >= 0; i--) putc(buf[i], ctx);
    }
}

/* ---------- 有符号整数 ---------- */

static void print_int(putc_fn putc, void *ctx, int64_t v,
                      int width, int zero_pad, int left_align) {
    char buf[24];
    int len = 0;
    int neg = 0;
    uint32_t u;

    if (v < 0) {
        neg = 1;
        u = (uint32_t)(-v);
    } else {
        u = (uint32_t)v;
    }

    if (u == 0) {
        buf[len++] = '0';
    } else {
        while (u > 0) {
            buf[len++] = '0' + (u % 10);
            u /= 10;
        }
    }

    int total = len + (neg ? 1 : 0);
    int pad = (width > total) ? width - total : 0;

    if (left_align) {
        if (neg) putc('-', ctx);
        for (int i = len - 1; i >= 0; i--) putc(buf[i], ctx);
        for (int i = 0; i < pad; i++) putc(' ', ctx);
    } else {
        if (zero_pad) {
            if (neg) putc('-', ctx);
            for (int i = 0; i < pad; i++) putc('0', ctx);
        } else {
            for (int i = 0; i < pad; i++) putc(' ', ctx);
            if (neg) putc('-', ctx);
        }
        for (int i = len - 1; i >= 0; i--) putc(buf[i], ctx);
    }
}

/* ---------- 指针 ---------- */

static void print_ptr(putc_fn putc, void *ctx, uint32_t v) {
    putc('0', ctx);
    putc('x', ctx);
    for (int i = 7; i >= 0; i--) {
        uint32_t d = (v >> (i * 4)) & 0xF;
        putc((d < 10) ? ('0' + d) : ('a' + d - 10), ctx);
    }
}

/* ---------- 核心 ---------- */

static void vformat(putc_fn putc, void *ctx, const char *fmt, va_list ap) {
    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++, ctx);
            continue;
        }
        fmt++;

        if (*fmt == '%') {
            putc('%', ctx);
            fmt++;
            continue;
        }

        int zero_pad = 0;
        int left_align = 0;
        for (;;) {
            if (*fmt == '0')      { zero_pad = 1; fmt++; }
            else if (*fmt == '-') { left_align = 1; fmt++; }
            else break;
        }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v = is_long ? (int64_t)va_arg(ap, long)
                                    : (int64_t)va_arg(ap, int);
                print_int(putc, ctx, v, width, zero_pad, left_align);
                break;
            }
            case 'u': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int);
                print_uint(putc, ctx, v, 10, 0, width, zero_pad, left_align);
                break;
            }
            case 'x': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int);
                print_uint(putc, ctx, v, 16, 0, width, zero_pad, left_align);
                break;
            }
            case 'X': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(ap, unsigned long)
                    : va_arg(ap, unsigned int);
                print_uint(putc, ctx, v, 16, 1, width, zero_pad, left_align);
                break;
            }
            case 'p': {
                uint32_t v = (uint32_t)(uintptr_t)va_arg(ap, void*);
                print_ptr(putc, ctx, v);
                break;
            }
            case 'c':
                putc((char)va_arg(ap, int), ctx);
                break;
            case 's': {
                const char *s = va_arg(ap, const char*);
                if (!s) s = "(null)";
                int len = 0;
                for (const char *p = s; *p; p++) len++;
                int pad = (width > len) ? width - len : 0;
                if (!left_align) for (int i = 0; i < pad; i++) putc(' ', ctx);
                while (*s) putc(*s++, ctx);
                if (left_align)  for (int i = 0; i < pad; i++) putc(' ', ctx);
                break;
            }
            default:
                putc('%', ctx);
                if (*fmt) putc(*fmt, ctx);
                break;
        }
        if (*fmt) fmt++;
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