#include "syscall.h"
#include "stdio.h"
#include "assert.h"

int cmp_int(const void *a, const void *b) {
    return *(const int*)a - *(const int*)b;
}

int main(int argc, char **argv) {
    (void)argv;
    int arr[] = {5, 2, 8, 1, 9, 3, 7, 4, 6, 0};
    qsort(arr, 10, sizeof(int), cmp_int);
    /* arr 应为 0..9 */

    assert(isdigit('5'));
    assert(!isdigit('a'));
    assert(toupper('a') == 'A');
}