#include "syscall.h"
#include "stdio.h"
#include "setjmp.h"
#include "string.h"

static jmp_buf env;

static void throw_error(void) {
    longjmp(env, 42);
}

int main(void) {
    dprintf(2, "test: setjmp/longjmp...\n");
    int ret = setjmp(env);
    if (ret == 0) {
        dprintf(2, "  first pass, calling longjmp...\n");
        throw_error();
        dprintf(2, "  ERROR: longjmp failed\n");
        return 1;
    } else {
        dprintf(2, "  caught longjmp with ret=%d\n", ret);
    }

    dprintf(2, "test: FILE* buffered I/O...\n");
    FILE *f = fopen("SYS:/TESTFILE.TXT", "w");
    if (!f) { dprintf(2, "  fopen w failed\n"); return 1; }
    fprintf(f, "Hello, %s! The number is %d.\n", "MethylUI", 123);
    fclose(f);

    f = fopen("SYS:/TESTFILE.TXT", "r");
    if (!f) { dprintf(2, "  fopen r failed\n"); return 1; }
    char buf[128];
    memset(buf, 0, sizeof(buf));
    unsigned int n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    dprintf(2, "  read back (%u bytes): %s", n, buf);

    dprintf(2, "all libc tests passed!\n");
    return 0;
}