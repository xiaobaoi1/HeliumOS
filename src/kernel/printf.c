#include <printf.h>
#include <serial.h>
#include <stdint.h>
#include <stdbool.h>

// ---------- 内部字符输出（绑定到串口） ----------
static void putc(char c) {
    // 换行符自动补充回车，确保终端显示正确
    if (c == '\n') {
        serial_write_char('\r');
    }
    serial_write_char(c);
}

// ---------- 数字打印核心 ----------
static void print_number(uint32_t num, int base, bool is_signed, bool uppercase) {
    // 处理有符号负数
    if (is_signed && (int32_t)num < 0) {
        putc('-');
        num = -(int32_t)num; // 转为正数
    }

    // 特殊情况：数字为 0
    if (num == 0) {
        putc('0');
        return;
    }

    // 缓冲区（最大 32 位二进制 32 位 + 终止符）
    char buf[33];
    int i = 0;

    while (num > 0) {
        uint32_t remainder = num % base;
        if (remainder < 10) {
            buf[i++] = '0' + remainder;
        } else {
            // 10->A, 11->B ...
            if (uppercase) {
                buf[i++] = 'A' + (remainder - 10);
            } else {
                buf[i++] = 'a' + (remainder - 10);
            }
        }
        num /= base;
    }

    // 逆序输出
    while (i > 0) {
        putc(buf[--i]);
    }
}

// ---------- kprintf 核心实现 ----------
void vkprintf(const char *fmt, va_list args) {
    if (!fmt) return;

    while (*fmt) {
        if (*fmt == '%') {
            fmt++; // 跳过 '%'

            // 处理转义：'%%' 输出 '%'
            if (*fmt == '%') {
                putc('%');
                fmt++;
                continue;
            }

            // 默认是 32 位无符号，用于 %x
            uint32_t num = 0;
            bool is_signed = false;
            bool uppercase = false;
            int base = 10;
            bool is_ptr = false;

            // 解析长度/类型
            switch (*fmt) {
                case 'c': {
                    // 字符
                    char c = (char)va_arg(args, int);
                    putc(c);
                    fmt++;
                    continue;
                }
                case 's': {
                    // 字符串
                    const char *str = va_arg(args, const char*);
                    if (str == NULL) str = "(null)";
                    while (*str) {
                        putc(*str++);
                    }
                    fmt++;
                    continue;
                }
                case 'd':
                case 'i': {
                    // 有符号十进制
                    is_signed = true;
                    base = 10;
                    num = va_arg(args, uint32_t);
                    fmt++;
                    break;
                }
                case 'u': {
                    // 无符号十进制
                    is_signed = false;
                    base = 10;
                    num = va_arg(args, uint32_t);
                    fmt++;
                    break;
                }
                case 'x': {
                    // 小写十六进制
                    uppercase = false;
                    base = 16;
                    num = va_arg(args, uint32_t);
                    fmt++;
                    break;
                }
                case 'X': {
                    // 大写十六进制
                    uppercase = true;
                    base = 16;
                    num = va_arg(args, uint32_t);
                    fmt++;
                    break;
                }
                case 'p': {
                    // 指针（按 32 位 16 进制输出，带 0x 前缀）
                    is_ptr = true;
                    uppercase = false;
                    base = 16;
                    num = va_arg(args, uint32_t);
                    fmt++;
                    putc('0');
                    putc('x');
                    // 补满 8 位：手动补前导零
                    int digits = 8; // 32 位 / 4
                    for (int i = 7; i >= 0; i--) {
                        uint8_t nibble = (num >> (i * 4)) & 0xF;
                        putc((nibble < 10) ? ('0' + nibble) : ('a' + nibble - 10));
                    }
                    continue; // 直接跳过 print_number，因为已处理
                }
                default: {
                    // 无法识别的格式，直接原样输出
                    putc('%');
                    putc(*fmt);
                    fmt++;
                    continue;
                }
            }

            // 常规数字输出（非指针）
            if (!is_ptr) {
                print_number(num, base, is_signed, uppercase);
            }

        } else {
            // 普通字符
            putc(*fmt);
            fmt++;
        }
    }
}

// ---------- 对外接口 ----------
void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vkprintf(fmt, args);
    va_end(args);
}