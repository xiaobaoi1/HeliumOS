/* test_mui_draw — 验证 methylui 的绘制原语。
 * 画一个窗口：背景 / 标题 / 边框 / 文本。 */
#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "wm_proto.h"
#include "shm_layout.h"
#include "methylui/mui.h"
#include <stdint.h>

int main(void) {
    int W = 400, H = 300;
    const char *shm_name = "mui-test-shm";
    const char *ev_name  = "mui-test-ev";

    int shm_size = shm_total_size(W, H);
    int shm_id = shm_open(shm_name, shm_size, 0);
    if (shm_id < 0) { printf("shm_open: %d\n", shm_id); return 1; }

    unsigned int addr = mmap(0, shm_size,
                             PROT_READ | PROT_WRITE,
                             MAP_SHARED, shm_id);
    if ((int)addr <= 0) { printf("mmap: %d\n", (int)addr); return 1; }

    struct shm_hdr *hdr = (struct shm_hdr*)addr;
    unsigned int *pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(pixels, 0, W * H * 4);
    hdr->counter = 0;
    hdr->cur_x0 = SHM_DIRTY_EMPTY; hdr->cur_y0 = 0;
    hdr->cur_x1 = 0; hdr->cur_y1 = 0;
    hdr->pub_x0 = SHM_DIRTY_EMPTY; hdr->pub_y0 = 0;
    hdr->pub_x1 = 0; hdr->pub_y1 = 0;

    /* ev pipe */
    pipe_unlink(ev_name);
    int fds[2];
    if (pipe_named(ev_name, fds) < 0) return 1;
    int ev_rd = fds[0];
    ipc_close(fds[1]);

    /* 创建 WM 窗口 */
    int inbox = pipe_open(WM_INBOX_NAME, 1);
    if (inbox < 0) { printf("wm-inbox: %d\n", inbox); return 1; }

    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC;
    req.type  = WM_REQ_CREATE;
    req.kind  = WM_KIND_NORMAL;
    req.x = 200;
    req.y = 150;
    req.w = W;
    req.h = H;
    req.min_w = 200;
    req.min_h = 150;
    req.max_w = 1600;
    req.max_h = 1200;
    req.owner_pid = getpid();
    strncpy(req.shm_name, shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, ev_name, sizeof(req.ev_name) - 1);
    strcpy(req.title, "MethylUI Draw Test");
    pipe_write(inbox, &req, sizeof(req));

    /* 字体 */
    mui_font_load();

    /* surface */
    struct mui_surface s;
    s.pixels = pixels;
    s.w = W;
    s.h = H;
    s.hdr = hdr;

    /* 绘制 */
    mui_fill(&s, 0, 0, W, H, 0x1A1A2E);

    /* 三个矩形 */
    mui_fill(&s, 20, 20, 100, 60, 0xAA0000);
    mui_fill(&s, 140, 20, 100, 60, 0x00AA00);
    mui_fill(&s, 260, 20, 100, 60, 0x0000AA);

    /* 边框 */
    mui_border(&s, 20, 100, 360, 80, 0xCCCCCC);

    /* 文本 */
    mui_text(&s, 30, 110, "MethylUI draw test", 0xFFFFFF, 0x1A1A2E);
    mui_text(&s, 30, 130, "fill / border / text", 0xCCCCCC, 0x1A1A2E);
    mui_text(&s, 30, 150, "ABC abc 0123 !@#$", 0x55FF55, 0x1A1A2E);

    /* 反色块 + 文字 */
    mui_fill(&s, 20, 200, 360, 30, 0x3A6EA5);
    mui_text(&s, 30, 207, "highlighted bar", 0xFFFFFF, 0x3A6EA5);

    /* 居中文本 */
    const char *center = "centered";
    int cw = mui_text_width(center);
    mui_text(&s, (W - cw) / 2, 250, center, 0xFFFF55, 0x1A1A2E);

    /* 发布 */
    hdr->cur_x0 = 0;
    hdr->cur_y0 = 0;
    hdr->cur_x1 = W;
    hdr->cur_y1 = H;
    hdr->counter++;

    /* 主循环 */
    for (;;) {
        struct wm_event ev;
        while (pipe_avail(ev_rd) >= (int)sizeof(ev)) {
            int n = pipe_read(ev_rd, &ev, sizeof(ev));
            if (n != sizeof(ev)) break;
            if (ev.type == WM_EV_CLOSE_REQUEST ||
                ev.type == WM_EV_CLOSE) {
                struct wm_destroy_request dr;
                dr.magic = WM_MAGIC;
                dr.type  = WM_REQ_DESTROY;
                dr.owner_pid = getpid();
                strncpy(dr.shm_name, shm_name, sizeof(dr.shm_name) - 1);
                pipe_write(inbox, &dr, sizeof(dr));
                return 0;
            }
        }
        sleep_ms(10);
    }

    return 0;
}