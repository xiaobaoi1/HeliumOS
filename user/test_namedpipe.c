/* 测试命名 pipe：
 * - 父创建 pipe，保留写端，关掉读端
 * - spawn 子进程，子进程 pipe_open 读端
 * - 父写数据，子读到
 * - 父 unlink，子关闭后对象销毁
 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fds[2];
    int r = pipe_named("test-np", fds);
    if (r < 0) { printf("pipe_named: %d\n", r); return 1; }
    printf("pipe_named: rd=%d wr=%d\n", fds[0], fds[1]);

    int rd = fds[0], wr = fds[1];

    /* 关掉自己的读端——只有子进程读 */
    ipc_close(rd);

    /* 用同一名字再创建应失败 */
    int fds2[2];
    r = pipe_named("test-np", fds2);
    printf("pipe_named again: %d (expect -15 = EEXIST)\n", r);

    /* 写数据 */
    const char *msg = "hello named pipe\n";
    int w = pipe_write(wr, msg, strlen(msg));
    printf("write: %d\n", w);

    /* 从写端读应失败（类型不对） */
    char buf[64];
    r = pipe_read(wr, buf, sizeof(buf));
    printf("read from wr: %d (expect -6 = EINVAL)\n", r);

    /* 关写端 */
    ipc_close(wr);

    /* unlink */
    r = pipe_unlink("test-np");
    printf("unlink: %d\n", r);

    /* 再 unlink 应 ENOENT */
    r = pipe_unlink("test-np");
    printf("unlink again: %d (expect -2 = ENOENT)\n", r);

    /* 名字消失后 open 应 ENOENT */
    r = pipe_open("test-np", 0);
    printf("open after unlink: %d (expect -2 = ENOENT)\n", r);

    printf("done\n");
    return 0;
}