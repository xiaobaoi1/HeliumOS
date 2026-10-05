#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("=== mmap tests ===\n");

    /* 1. 匿名 mmap */
    unsigned int addr = mmap(0, 4096, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1);
    printf("anon mmap: 0x%x\n", addr);
    if ((int)addr > 0) {
        char *p = (char*)addr;
        p[0] = 'A';
        p[4095] = 'Z';
        printf("  write ok: %c %c\n", p[0], p[4095]);
        munmap(addr, 4096);
        printf("  munmap ok\n");
    }

    /* 2. 共享内存 */
    int shm1 = shm_open("/test", 4096, 0);
    printf("shm_open: %d\n", shm1);

    unsigned int s1 = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED, shm1);
    printf("shared mmap #1: 0x%x\n", s1);

    /* 同进程再映射一次（模拟第二进程） */
    unsigned int s2 = mmap(0, 4096, PROT_READ | PROT_WRITE,
                           MAP_SHARED, shm1);
    printf("shared mmap #2: 0x%x\n", s2);

    if ((int)s1 > 0 && (int)s2 > 0) {
        char *a = (char*)s1;
        char *b = (char*)s2;
        a[0] = 'H';
        printf("  A wrote 'H', B reads '%c' (expect H)\n", b[0]);
        b[100] = 'W';
        printf("  B wrote 'W', A reads '%c' (expect W)\n", a[100]);
    }

    shm_unlink("/test");

    printf("=== done ===\n");

        /* A2: malloc 大分配 */
    printf("\n=== A2 malloc test ===\n");
    void *big = malloc(128 * 1024);
    if (!big) {
        printf("malloc(128K) FAIL\n");
        return 1;
    }
    memset(big, 0xAB, 128 * 1024);
    if (((unsigned char*)big)[0] != 0xAB ||
        ((unsigned char*)big)[128*1024 - 1] != 0xAB) {
        printf("big write FAIL\n");
        return 1;
    }
    free(big);
    printf("malloc(128K) + free: PASS\n");

    /* 小分配仍然走 brk */
    void *small = malloc(100);
    if (!small) { printf("malloc(100) FAIL\n"); return 1; }
    free(small);
    printf("malloc(100) + free: PASS\n");
    return 0;
}