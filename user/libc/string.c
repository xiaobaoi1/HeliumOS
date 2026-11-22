#include "string.h"
#include "stdlib.h"

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char*)dest;
    const unsigned char *s = (const unsigned char*)src;
    if (d == s || n == 0) return dest;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--) d[i-1] = s[i-1];
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char*)a;
    const unsigned char *y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char *strchr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char*)s;
        s++;
    }
    return (c == '\0') ? (char*)s : NULL;
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++) != '\0');
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    size_t i = 0;
    while (i < n && src[i]) { d[i] = src[i]; i++; }
    d[i] = '\0';
    return dest;
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if ((char)c == '\0') return (char*)s;
    return (char*)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char*)haystack;
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char*)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        for (const char *a = accept; *a; a++) {
            if (*s == *a) return (char*)s;
        }
        s++;
    }
    return NULL;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    while (n--) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        if (!ca) return 0;
    }
    return 0;
}

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = (char*)malloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = '\0';
    }
    return p;
}

size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n]) {
        for (const char *r = reject; *r; r++)
            if (s[n] == *r) return n;
        n++;
    }
    return n;
}

char *strtok(char *str, const char *delim) {
    static char *next = NULL;

    if (str) next = str;
    if (!next) return NULL;

    /* 跳过前导分隔符 */
    while (*next) {
        int is_delim = 0;
        for (const char *d = delim; *d; d++)
            if (*next == *d) { is_delim = 1; break; }
        if (!is_delim) break;
        next++;
    }
    if (!*next) { next = NULL; return NULL; }

    char *token = next;
    while (*next) {
        for (const char *d = delim; *d; d++) {
            if (*next == *d) {
                *next = '\0';
                next++;
                return token;
            }
        }
        next++;
    }
    next = NULL;
    return token;
}

size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n]) {
        int found = 0;
        for (const char *a = accept; *a; a++)
            if (s[n] == *a) { found = 1; break; }
        if (!found) break;
        n++;
    }
    return n;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strerror(int errnum) {
    switch (errnum) {
        case  0: return "OK";
        case -1: return "Operation not permitted";
        case -2: return "No such file or directory";
        case -3: return "I/O error";
        case -4: return "Out of memory";
        case -5: return "Bad address";
        case -6: return "Invalid argument";
        case -7: return "Resource busy";
        case -8: return "No space left";
        case -9: return "Not implemented";
        case -10: return "Try again";
        case -11: return "Interrupted";
        case -12: return "Range error";
        case -13: return "Not a directory";
        case -14: return "Is a directory";
        case -15: return "File exists";
        case -16: return "Directory not empty";
        case -17: return "Broken pipe";
        case -18: return "No child processes";
        case -19: return "Exec format error";
        case -20: return "Name too long";
        case -21: return "Cross-device link";
        case -22: return "Address in use";
        case -23: return "Timed out";
        case -24: return "Connection refused";
        case -25: return "Bad file descriptor";
        default:  return "Unknown error";
    }
}