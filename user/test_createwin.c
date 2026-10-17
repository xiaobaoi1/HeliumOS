/* 测试窗口创建 API。
 * 应用自己画到 shm，WM 显示。
 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "wm_proto.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int W = 400, H = 300;
    int shm_size = 8 + W * H * 4;

    /* 1. shm + mmap */
    int shm_id = shm_open("mywin-shm", shm_size, 0);
    if (shm_id < 0) { printf("shm_open: %d\n", shm_id); return 1; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { printf("mmap: %d\n", (int)addr); return 1; }

    uint64_t *counter = (uint64_t*)addr;
    unsigned int *pixels = (unsigned int*)(addr + 8);

    /* 红色背景 + 绿色中心方块 */
    for (int i = 0; i < W * H; i++) pixels[i] = 0xFF0000;
    for (int y = H/3; y < 2*H/3; y++)
        for (int x = W/3; x < 2*W/3; x++)
            pixels[y * W + x] = 0x00FF00;
    *counter = 1;

    /* 2. ev pipe（保留读端） */
    int fds[2];
    if (pipe_named("mywin-ev", fds) < 0) {
        printf("pipe_named failed\n"); return 1;
    }
    int ev_rd = fds[0];
    ipc_close(fds[1]);

    /* 3. 发创建请求 */
    int inbox = pipe_open(WM_INBOX_NAME, 1);
    if (inbox < 0) { printf("wm-inbox: %d\n", inbox); return 1; }

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type = WM_REQ_CREATE;
    req.w = W;
    req.h = H;
    req.owner_pid = getpid();
    strcpy(req.shm_name, "mywin-shm");
    strcpy(req.ev_name, "mywin-ev");
    strcpy(req.title, "Test Window");

    pipe_write(inbox, &req, sizeof(req));
    printf("window created, waiting 10s...\n");

    /* 4. 期间可以接收键盘（如果有焦点）——测试用，只 sleep */
    sleep_ms(10000);

    printf("exiting...\nWM will close the window.\n");
    return 0;
}