#include <printf.h>
#include <serial.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <tty.h>

/* ---------- 输出后端 ---------- */

typedef void (*putc_fn)(char c, void *ctx);

static void putc_console(char c, void *ctx) {
    (void)ctx;
    serial_write_char(c);
    tty_write_char(c);
}

struct str_ctx {
    char  *buf;
    size_t pos;
    size_t limit;
};

static void putc_str(char c, void *ctx) {
    struct str_ctx *sc = (struct str_ctx*)ctx;
    if (sc->pos + 1 < sc->limit) sc->buf[sc->pos] = c;
    sc->pos++;
}

/* ---------- 无符号整数 ---------- */

/* 64 位除以小整数（base ≤ 16），只用 32 位运算。
 * 避免内核链接时产生 __udivdi3 / __umoddi3 依赖。 */
static uint32_t divmod64(uint64_t *v, uint32_t base) {
    uint64_t x = *v;
    uint64_t q = 0;
    uint32_t r = 0;

    int top = -1;
    for (int i = 63; i >= 0; i--) {
        if ((x >> i) & 1u) { top = i; break; }
    }

    for (int i = top; i >= 0; i--) {
        r = (r << 1) | (uint32_t)((x >> i) & 1u);
        q <<= 1;
        if (r >= base) { r -= base; q |= 1; }
    }

    *v = q;
    return r;
}

static void print_uint(putc_fn putc, void *ctx, uint64_t v, int base,
                       int uppercase, int width, int zero_pad, int left_align) {
    char buf[24];
    int len = 0;
    const char *digits = uppercase ? "0123456789ABCDEF"
                                   : "0123456789abcdef";

    if (v == 0) {
        buf[len++] = '0';
    } else {
        while (v > 0) {
            uint32_t d = divmod64(&v, (uint32_t)base);
            buf[len++] = digits[d];
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
    uint64_t u;

    if (v < 0) {
        neg = 1;
        u = ~(uint64_t)v + 1;
    } else {
        u = (uint64_t)v;
    }

    if (u == 0) {
        buf[len++] = '0';
    } else {
        while (u > 0) {
            uint32_t d = divmod64(&u, 10);
            buf[len++] = '0' + d;
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

static void vformat(putc_fn putc, void *ctx, const char *fmt, va_list args) {
    if (!fmt) return;

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

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            while (*fmt >= '0' && *fmt <= '9') {
                precision = precision * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* 长度修饰符：l / ll / h / hh / z / j
         * 32 位下 size_t = unsigned int，intmax_t = long long。
         * 必须消费正确的字节数，否则 va_arg 栈错位。 */
        int is_long = 0;
        int is_long_long = 0;
        int is_size = 0;

        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') { is_long_long = 1; fmt++; }
            else             { is_long = 1; }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;   /* hh 和 h 在 32 位下 va_arg 都是 int（提升） */
            /* short 在变参中被提升为 int，无需特殊处理 */
        } else if (*fmt == 'z') {
            is_size = 1; fmt++;
        } else if (*fmt == 'j') {
            is_long_long = 1; fmt++;
        }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v;
                if (is_long_long)      v = va_arg(args, long long);
                else if (is_long)      v = va_arg(args, long);
                else if (is_size)      v = (int64_t)va_arg(args, unsigned int);
                else                   v = va_arg(args, int);
                print_int(putc, ctx, v, width, zero_pad, left_align);
                break;
            }
            case 'u': {
                uint64_t v;
                if (is_long_long)      v = va_arg(args, unsigned long long);
                else if (is_long)      v = (uint64_t)va_arg(args, unsigned long);
                else                   v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 10, 0, width, zero_pad, left_align);
                break;
            }
            case 'x': {
                uint64_t v;
                if (is_long_long)      v = va_arg(args, unsigned long long);
                else if (is_long)      v = (uint64_t)va_arg(args, unsigned long);
                else                   v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 16, 0, width, zero_pad, left_align);
                break;
            }
            case 'X': {
                uint64_t v;
                if (is_long_long)      v = va_arg(args, unsigned long long);
                else if (is_long)      v = (uint64_t)va_arg(args, unsigned long);
                else                   v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 16, 1, width, zero_pad, left_align);
                break;
            }
            case 'p': {
                uint32_t v = (uint32_t)(uintptr_t)va_arg(args, void*);
                print_ptr(putc, ctx, v);
                break;
            }
            case 'c':
                putc((char)va_arg(args, int), ctx);
                break;
            case 's': {
                const char *s = va_arg(args, const char*);
                if (!s) s = "(null)";
                int len = 0;
                for (const char *p = s; *p; p++) len++;
                if (precision >= 0 && len > precision) len = precision;
                int pad = (width > len) ? width - len : 0;
                if (!left_align) for (int i = 0; i < pad; i++) putc(' ', ctx);
                for (int i = 0; i < len; i++) putc(s[i], ctx);
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

void vkprintf(const char *fmt, va_list args) {
    vformat(putc_console, NULL, fmt, args);
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vformat(putc_console, NULL, fmt, args);
    va_end(args);
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