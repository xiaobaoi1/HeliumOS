#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

int main(void) {
    dprintf(2, "test: big malloc...\n");
    
    /* 测试 1: 分配 10KB（跨 3 页） */
    char *p1 = (char*)malloc(10 * 1024);
    if (!p1) { dprintf(2, "  malloc 10K FAIL\n"); return 1; }
    memset(p1, 0xAA, 10 * 1024);
    if (p1[0] != (char)0xAA || p1[10239] != (char)0xAA) {
        dprintf(2, "  write 10K FAIL\n"); return 1;
    }
    free(p1);
    dprintf(2, "  10K PASS\n");
    
    /* 测试 2: 分配 500KB（TCC 级别） */
    char *p2 = (char*)malloc(500 * 1024);
    if (!p2) { dprintf(2, "  malloc 500K FAIL\n"); return 1; }
    memset(p2, 0xBB, 500 * 1024);
    if (p2[0] != (char)0xBB || p2[500 * 1024 - 1] != (char)0xBB) {
        dprintf(2, "  write 500K FAIL\n"); return 1;
    }
    free(p2);
    dprintf(2, "  500K PASS\n");
    
    /* 测试 3: 多次分配释放 */
    void *ptrs[10];
    for (int i = 0; i < 10; i++) {
        ptrs[i] = malloc((i + 1) * 16 * 1024);
        if (!ptrs[i]) { dprintf(2, "  loop %d FAIL\n", i); return 1; }
        memset(ptrs[i], i, (i + 1) * 16 * 1024);
    }
    for (int i = 0; i < 10; i++) {
        free(ptrs[i]);
    }
    dprintf(2, "  loop 160K PASS\n");

    dprintf(2, "all heap tests passed!\n");
    return 0;
}