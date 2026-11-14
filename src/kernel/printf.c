#include <printf.h>
#include <serial.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <tty.h>

/* ---------- 输出后端 ---------- */

typedef void (*putc_fn)(char c, void *ctx);

/* ---------- 日志环（供 panic dump） ---------- */

#define LOG_RING_SIZE  8192
static char             g_log_ring[LOG_RING_SIZE];
static volatile uint32_t g_log_head = 0;
static volatile uint8_t  g_log_wrapped = 0;

static void log_ring_putc(char c) {
    g_log_ring[g_log_head] = c;
    g_log_head = (g_log_head + 1) % LOG_RING_SIZE;
    if (g_log_head == 0) g_log_wrapped = 1;
}

int log_ring_size(void) {
    return g_log_wrapped ? LOG_RING_SIZE : (int)g_log_head;
}

char log_ring_at(int idx) {
    int n = log_ring_size();
    if (idx < 0 || idx >= n) return '\0';
    int start = g_log_wrapped ? (int)g_log_head : 0;
    return g_log_ring[(start + idx) % LOG_RING_SIZE];
}

static void putc_console(char c, void *ctx) {
    (void)ctx;
    log_ring_putc(c);
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

/* ---------- 无符号整数（32位，避免64位除法） ---------- */

static void print_uint(putc_fn putc, void *ctx, uint32_t v, int base,
                       int uppercase, int width, int zero_pad, int left_align,
                       int hash_sign) {
    char buf[32];
    int len = 0;
    const char *digits = uppercase ? "0123456789ABCDEF"
                                   : "0123456789abcdef";

    if (v == 0) {
        if (hash_sign && base == 16) buf[len++] = '0';
        buf[len++] = '0';
    } else {
        while (v > 0) {
            buf[len++] = digits[v % base];
            v /= base;
        }
        if (hash_sign && base == 16) {
            if (uppercase) { buf[len++] = 'X'; buf[len++] = '0'; }
            else           { buf[len++] = 'x'; buf[len++] = '0'; }
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

/* ---------- 有符号整数（32位） ---------- */

static void print_int(putc_fn putc, void *ctx, int32_t v,
                      int width, int zero_pad, int left_align,
                      int plus_sign, int space_sign) {
    char buf[24];
    int len = 0;
    int neg = (v < 0);
    uint32_t u;

    if (neg) {
        u = (uint32_t)(-(v + 1)) + 1;
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

    int sign_len = neg ? 1 : (plus_sign ? 1 : (space_sign ? 1 : 0));
    int total = len + sign_len;
    int pad = (width > total) ? width - total : 0;

    if (left_align) {
        if (neg) putc('-', ctx);
        else if (plus_sign) putc('+', ctx);
        else if (space_sign) putc(' ', ctx);
        for (int i = len - 1; i >= 0; i--) putc(buf[i], ctx);
        for (int i = 0; i < pad; i++) putc(' ', ctx);
    } else {
        if (zero_pad) {
            if (neg) putc('-', ctx);
            else if (plus_sign) putc('+', ctx);
            else if (space_sign) putc(' ', ctx);
            for (int i = 0; i < pad; i++) putc('0', ctx);
        } else {
            for (int i = 0; i < pad; i++) putc(' ', ctx);
            if (neg) putc('-', ctx);
            else if (plus_sign) putc('+', ctx);
            else if (space_sign) putc(' ', ctx);
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
        int plus_sign = 0;
        int space_sign = 0;
        int hash_sign = 0;

        for (;;) {
            if (*fmt == '0')      { zero_pad = 1; fmt++; }
            else if (*fmt == '-') { left_align = 1; fmt++; }
            else if (*fmt == '+') { plus_sign = 1; fmt++; }
            else if (*fmt == ' ') { space_sign = 1; fmt++; }
            else if (*fmt == '#') { hash_sign = 1; fmt++; }
            else break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) {
                left_align = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                fmt++;
            } else {
                precision = 0;
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
            if (precision < 0) precision = -1;
        }

        int is_long = 0;
        int is_longlong = 0;
        int is_size = 0;
        if (*fmt == 'l') {
            is_long = 1; fmt++;
            if (*fmt == 'l') { is_longlong = 1; fmt++; }
        } else if (*fmt == 'z') {
            is_size = 1; fmt++;
        }

        switch (*fmt) {
            case 'd':
            case 'i': {
                int32_t v;
                if (is_longlong)      v = (int32_t)va_arg(args, long long);
                else if (is_long)     v = (int32_t)va_arg(args, long);
                else if (is_size)     v = (int32_t)va_arg(args, size_t);
                else                  v = va_arg(args, int);
                print_int(putc, ctx, v, width, zero_pad, left_align, plus_sign, space_sign);
                break;
            }
            case 'u': {
                uint32_t v;
                if (is_longlong)      v = (uint32_t)va_arg(args, unsigned long long);
                else if (is_long)     v = (uint32_t)va_arg(args, unsigned long);
                else if (is_size)     v = (uint32_t)va_arg(args, size_t);
                else                  v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 10, 0, width, zero_pad, left_align, hash_sign);
                break;
            }
            case 'x': {
                uint32_t v;
                if (is_longlong)      v = (uint32_t)va_arg(args, unsigned long long);
                else if (is_long)     v = (uint32_t)va_arg(args, unsigned long);
                else if (is_size)     v = (uint32_t)va_arg(args, size_t);
                else                  v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 16, 0, width, zero_pad, left_align, hash_sign);
                break;
            }
            case 'X': {
                uint32_t v;
                if (is_longlong)      v = (uint32_t)va_arg(args, unsigned long long);
                else if (is_long)     v = (uint32_t)va_arg(args, unsigned long);
                else if (is_size)     v = (uint32_t)va_arg(args, size_t);
                else                  v = va_arg(args, unsigned int);
                print_uint(putc, ctx, v, 16, 1, width, zero_pad, left_align, hash_sign);
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

                if (precision >= 0) {
                    while (len < precision && s[len]) len++;
                } else {
                    while (s[len]) len++;
                }

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