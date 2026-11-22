#include <stddef.h>

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

static void qsort_rec(char *base, int lo, int hi, size_t size,
                      int (*cmp)(const void *, const void *)) {
    if (lo >= hi) return;

    /* pivot = base[hi] */
    char *pivot = base + (size_t)hi * size;
    int i = lo - 1;
    for (int j = lo; j < hi; j++) {
        if (cmp(base + (size_t)j * size, pivot) <= 0) {
            i++;
            if (i != j) swap_bytes(base + (size_t)i * size,
                                   base + (size_t)j * size, size);
        }
    }
    i++;
    if (i != hi) swap_bytes(base + (size_t)i * size,
                            base + (size_t)hi * size, size);

    qsort_rec(base, lo, i - 1, size, cmp);
    qsort_rec(base, i + 1, hi, size, cmp);
}

void qsort(void *base, size_t n, size_t size,
           int (*cmp)(const void *, const void *)) {
    if (n < 2 || size == 0) return;
    qsort_rec((char*)base, 0, (int)n - 1, size, cmp);
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *)) {
    const char *b = (const char *)base;
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void *elem = b + mid * size;
        int c = cmp(key, elem);
        if (c == 0) return (void *)elem;
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}