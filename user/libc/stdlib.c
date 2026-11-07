#include "stdlib.h"
#include <stddef.h>
#include <stdint.h>

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





unsigned long strtoul(const char *s, char **endptr, int base) {
    return (unsigned long)strtol(s, endptr, base);
}
unsigned long long strtoull(const char *s, char **endptr, int base) {
    return (unsigned long long)strtoul(s, endptr, base);
}
long long strtoll(const char *s, char **endptr, int base) {
    return (long long)strtol(s, endptr, base);
}
double strtod(const char *nptr, char **endptr) {
    return (double)strtol(nptr, endptr, 10);
}
float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}
long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}
long double ldexpl(long double x, int exp) {
    return x; /* 简易兜底 */
}

unsigned long __isoc23_strtoul(const char *s, char **endptr, int base) {
    return strtoul(s, endptr, base);
}
unsigned long long __isoc23_strtoull(const char *s, char **endptr, int base) {
    return strtoull(s, endptr, base);
}
long __isoc23_strtol(const char *s, char **endptr, int base) {
    return strtol(s, endptr, base);
}
long long __isoc23_strtoll(const char *s, char **endptr, int base) {
    return strtoll(s, endptr, base);
}

/* ---------- 64 位无符号除法运行时（TCC 生成的 __udivdi3/__umoddi3） ----------
 * 64 位 `%` / `/` 被 TCC 编译成这两个函数的调用，不内联。
 * 算法：shift-subtract 长除法，65 位余数用 r_hi 标志。 */
uint64_t __udivdi3(uint64_t n, uint64_t d) {
    if (d == 0) return 0;
    if (d > n)  return 0;
    if (d == n) return 1;

    /* 32/32 快速路径 */
    if ((n >> 32) == 0 && (d >> 32) == 0)
        return (uint32_t)n / (uint32_t)d;

    uint64_t q = 0;
    uint64_t r = 0;
    for (int bit = 63; bit >= 0; bit--) {
        uint32_t next = (uint32_t)((n >> bit) & 1);
        uint32_t r_hi = (uint32_t)(r >> 63) & 1;
        uint64_t r_lo = (r << 1) | next;
        if (r_hi || r_lo >= d) {
            r = r_lo - d;           /* uint64 减法，自动处理借位 */
            q |= (1ULL << bit);
        } else {
            r = r_lo;
        }
    }
    return q;
}

uint64_t __umoddi3(uint64_t n, uint64_t d) {
    return n - __udivdi3(n, d) * d;
}