#include <printf.h>
#include <serial.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- 输出后端 ---------- */
/* 当前绑定到串口。将来接多后端时，把 putc 改成函数指针。 */
static void putc(char c) {
    serial_write_char(c);
}

/* ---------- 无符号整数格式化 ---------- */

static void print_uint(uint32_t v, int base, int uppercase,
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
        for (int i = len - 1; i >= 0; i--) putc(buf[i]);
        for (int i = 0; i < pad; i++) putc(' ');
    } else {
        char pc = zero_pad ? '0' : ' ';
        for (int i = 0; i < pad; i++) putc(pc);
        for (int i = len - 1; i >= 0; i--) putc(buf[i]);
    }
}

/* ---------- 有符号整数格式化 ---------- */

static void print_int(int64_t v, int width, int zero_pad, int left_align) {
    char buf[24];
    int len = 0;
    int neg = 0;
    uint32_t u;

    if (v < 0) {
        neg = 1;
        /* int64_t 上取负，安全处理 INT32_MIN */
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
        if (neg) putc('-');
        for (int i = len - 1; i >= 0; i--) putc(buf[i]);
        for (int i = 0; i < pad; i++) putc(' ');
    } else {
        if (zero_pad) {
            /* 零填充时符号位在前：-0123 */
            if (neg) putc('-');
            for (int i = 0; i < pad; i++) putc('0');
        } else {
            for (int i = 0; i < pad; i++) putc(' ');
            if (neg) putc('-');
        }
        for (int i = len - 1; i >= 0; i--) putc(buf[i]);
    }
}

/* ---------- 指针：0x + 8 位十六进制 ---------- */

static void print_ptr(uint32_t v) {
    putc('0');
    putc('x');
    for (int i = 7; i >= 0; i--) {
        uint32_t d = (v >> (i * 4)) & 0xF;
        putc((d < 10) ? ('0' + d) : ('a' + d - 10));
    }
}

/* ---------- 核心 ---------- */

void vkprintf(const char *fmt, va_list args) {
    if (!fmt) return;

    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++);
            continue;
        }
        fmt++;  /* 跳过 '%' */

        if (*fmt == '%') {
            putc('%');
            fmt++;
            continue;
        }

        /* 解析 flags：0 / - */
        int zero_pad = 0;
        int left_align = 0;
        for (;;) {
            if (*fmt == '0')      { zero_pad = 1; fmt++; }
            else if (*fmt == '-') { left_align = 1; fmt++; }
            else break;
        }

        /* 解析宽度 */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* 解析长度：l */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        /* 解析类型 */
        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t v = is_long ? (int64_t)va_arg(args, long)
                                    : (int64_t)va_arg(args, int);
                print_int(v, width, zero_pad, left_align);
                break;
            }
            case 'u': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(args, unsigned long)
                    : va_arg(args, unsigned int);
                print_uint(v, 10, 0, width, zero_pad, left_align);
                break;
            }
            case 'x': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(args, unsigned long)
                    : va_arg(args, unsigned int);
                print_uint(v, 16, 0, width, zero_pad, left_align);
                break;
            }
            case 'X': {
                uint32_t v = is_long
                    ? (uint32_t)va_arg(args, unsigned long)
                    : va_arg(args, unsigned int);
                print_uint(v, 16, 1, width, zero_pad, left_align);
                break;
            }
            case 'p': {
                uint32_t v = (uint32_t)(uintptr_t)va_arg(args, void*);
                print_ptr(v);
                break;
            }
            case 'c':
                putc((char)va_arg(args, int));
                break;
            case 's': {
                const char *s = va_arg(args, const char*);
                if (!s) s = "(null)";
                int len = 0;
                for (const char *p = s; *p; p++) len++;
                int pad = (width > len) ? width - len : 0;
                if (!left_align) for (int i = 0; i < pad; i++) putc(' ');
                while (*s) putc(*s++);
                if (left_align)  for (int i = 0; i < pad; i++) putc(' ');
                break;
            }
            default:
                /* 未知修饰符：原样输出，保持可见 */
                putc('%');
                if (*fmt) putc(*fmt);
                break;
        }
        if (*fmt) fmt++;
    }
}

/* ---------- 对外接口 ---------- */

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}