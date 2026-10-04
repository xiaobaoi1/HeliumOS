#include "stdlib.h"
#include <stddef.h>

/* ---------- 字符串转数字 ---------- */

static int digit_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

long strtol(const char *s, char **endptr, int base) {
    if (!s) {
        if (endptr) *endptr = (char*)s;
        return 0;
    }

    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' ||
           *p == '\r' || *p == '\f' || *p == '\v') {
        p++;
    }

    int neg = 0;
    if (*p == '+' || *p == '-') {
        neg = (*p == '-');
        p++;
    }

    if (base == 0) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        } else if (p[0] == '0') {
            base = 8;
            p++;
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    }

    long val = 0;
    while (*p) {
        int d = digit_val(*p);
        if (d < 0 || d >= base) break;
        val = val * base + d;
        p++;
    }

    if (endptr) *endptr = (char*)p;
    return neg ? -val : val;
}

int atoi(const char *s) {
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s) {
    return strtol(s, NULL, 10);
}

/* ---------- 绝对值 ---------- */

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

/* ---------- 随机数 ----------
 * 简单 LCG。参数取自 glibc 旧实现。
 * 无种子时默认 1，保证可复现。 */

static unsigned long g_rand_state = 1;

void srand(unsigned int seed) {
    g_rand_state = seed ? seed : 1;
}

int rand(void) {
    g_rand_state = g_rand_state * 1103515245u + 12345u;
    return (int)((g_rand_state >> 16) & 0x7FFF);
}