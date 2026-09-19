#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "errno.h"

static int test_count = 0;
static int pass_count  = 0;

static void report(const char *name, int ret, int expect) {
    test_count++;
    if (ret == expect) {
        printf("  PASS: %s = %d\n", name, ret);
        pass_count++;
    } else {
        printf("  FAIL: %s = %d (expected %d)\n", name, ret, expect);
    }
}

void _start(void) {
    printf("\n=== bad pointer test ===\n");
    printf("All should reject with EFAULT(%d)\n\n", EFAULT);

    /* 低地址野指针 */
    const char *bad_low  = (const char*)0x00001000;
    /* 内核空间地址（用户态不可访问） */
    const char *bad_kern = (const char*)0x00100000;

    /* 1. fs_open 路径 */
    report("fs_open(low)",   fs_open(bad_low,  FS_O_RDONLY), EFAULT);
    report("fs_open(kern)",  fs_open(bad_kern, FS_O_RDONLY), EFAULT);

    /* 2. fs_opendir */
    report("fs_opendir(low)",  fs_opendir(bad_low),  EFAULT);
    report("fs_opendir(kern)", fs_opendir(bad_kern), EFAULT);

    /* 3. fs_chdir */
    report("fs_chdir(low)",  chdir(bad_low),  EFAULT);
    report("fs_chdir(kern)", chdir(bad_kern), EFAULT);

    /* 4. fs_read 缓冲区 */
    int fd = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    if (fd >= 0) {
        report("fs_read(low buf)",   fs_read(fd, (void*)bad_low,  16), EFAULT);
        report("fs_read(kern buf)",  fs_read(fd, (void*)bad_kern, 16), EFAULT);
        fs_close(fd);
    } else {
        printf("  SKIP: cannot open SHELL.ELF (%d)\n", fd);
    }

    /* 5. fs_readdir 结构指针 */
    int dirfd = fs_opendir("SYS:/");
    if (dirfd >= 0) {
        report("fs_readdir(low)",  fs_readdir(dirfd, (void*)bad_low),  EFAULT);
        report("fs_readdir(kern)", fs_readdir(dirfd, (void*)bad_kern), EFAULT);
        fs_closedir(dirfd);
    } else {
        printf("  SKIP: cannot open SYS:/ (%d)\n", dirfd);
    }

    /* 6. getcwd 缓冲区 */
    report("getcwd(low)",  getcwd((char*)bad_low,  32), EFAULT);
    report("getcwd(kern)", getcwd((char*)bad_kern, 32), EFAULT);

    /* 7. write 缓冲区 */
    report("write(low)",   write(1, bad_low, 4),  EFAULT);
    report("write(kern)",  write(1, bad_kern, 4), EFAULT);

    /* 8. read 缓冲区（keyboard 非阻塞，看是否先检查指针） */
    report("read(low)",  read(0, (void*)bad_low,  4), EFAULT);
    report("read(kern)", read(0, (void*)bad_kern, 4), EFAULT);

    /* 9. spawn 路径 */
    report("spawn(low)",  spawn(bad_low),  EFAULT);
    report("spawn(kern)", spawn(bad_kern), EFAULT);

    /* 10. dev_open/read/write */
    int kb = dev_open(1 /* KEYBOARD */, 0);
    if (kb >= 0) {
        report("dev_read(low)",  dev_read(kb, (void*)bad_low,  4), EFAULT);
        report("dev_read(kern)", dev_read(kb, (void*)bad_kern, 4), EFAULT);
        report("dev_write(low)", dev_write(kb, bad_low,  4),  EFAULT);
        report("dev_write(kern)",dev_write(kb, bad_kern, 4),  EFAULT);
        dev_close(kb);
    } else {
        printf("  SKIP: cannot open keyboard (%d)\n", kb);
    }

    /* 11. 合法调用应该仍然成功（回归测试） */
    printf("\n--- regression (valid calls) ---\n");
    fd = fs_open("SYS:/SHELL.ELF", FS_O_RDONLY);
    report("fs_open(valid)", fd >= 0 ? 0 : -1, 0);
    if (fd >= 0) fs_close(fd);

    char cwd[64];
    report("getcwd(valid)", getcwd(cwd, sizeof(cwd)) > 0 ? 0 : -1, 0);

    printf("\n=== %d / %d passed ===\n", pass_count, test_count);
    _exit(test_count - pass_count);
}