#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include <stdint.h>

static int spawn_service(const char *path, char *const argv[],
                         char *const envp[], uint32_t child_class,
                         uint32_t child_caps) {
    struct spawn_params sp;
    memset(&sp, 0, sizeof(sp));
    sp.size = sizeof(sp);
    sp.in_fd = SPAWN_FD_INHERIT;
    sp.out_fd = SPAWN_FD_INHERIT;
    sp.err_fd = SPAWN_FD_INHERIT;
    sp.in_pipe = sp.out_pipe = sp.err_pipe = -1;
    sp.in_pty = sp.out_pty = sp.err_pty = -1;
    sp.envp = (uint32_t)envp;
    sp.child_class = child_class;
    sp.child_caps = child_caps;
    return proc_spawn(path, argv, &sp);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    char screen_w[32] = "SCREEN_W=1024";
    char screen_h[32] = "SCREEN_H=768";
    const char *w = getenv("SCREEN_W");
    const char *h = getenv("SCREEN_H");
    if (w) snprintf(screen_w, sizeof(screen_w), "SCREEN_W=%s", w);
    if (h) snprintf(screen_h, sizeof(screen_h), "SCREEN_H=%s", h);
    char *desk_env[] = { screen_w, screen_h, NULL };
    char *shell_env[] = { "PATH=SYS:/", "HOME=SYS:/", "TERM=helium", NULL };

    char *wm_argv[] = { "WM", NULL };
    char *desk_argv[] = { "DESKTOP", NULL };
    char *dhcp_argv[] = { "DHCPCLIENT", NULL };
    char *shell_argv[] = { "SHELL", NULL };

    int wm_h = -1;
    int desk_h = -1;
    int dhcp_h = -1;
    int shell_h = -1;

    /* WM 必须先启动并 claim fb/input。 */
    wm_h = spawn_service("SYS:/WM", wm_argv, NULL,
                         CLASS_SERVICE, CAP_DISPLAY | CAP_MMIO);
    dprintf(2, "init: WM handle=%d\n", wm_h);
    sys_sleep_ms(100);

    desk_h = spawn_service("SYS:/DESKTOP", desk_argv, desk_env,
                           CLASS_SERVICE, 0);
    dprintf(2, "init: DESKTOP handle=%d\n", desk_h);
    sys_sleep_ms(100);

    dhcp_h = spawn_service("SYS:/DHCPCLIENT", dhcp_argv, NULL,
                           CLASS_SERVICE, 0);
    dprintf(2, "init: DHCP client handle=%d\n", dhcp_h);

    /* 保留一个默认 TTY 上的 shell；桌面终端仍可另行打开。 */
    shell_h = spawn_service("SYS:/SHELL", shell_argv, shell_env,
                            CLASS_USER, 0);
    dprintf(2, "init: SHELL handle=%d\n", shell_h);

    /* init 作为 PID 1：服务退出后重新拉起，避免桌面/终端永久消失。 */
    for (;;) {
        int status = 0;
        if (wm_h >= 0 && proc_wait(wm_h, &status, 5) == OK) {
            proc_close(wm_h);
            dprintf(2, "init: WM exited status=%d, restart\n", status);
            wm_h = spawn_service("SYS:/WM", wm_argv, NULL,
                                 CLASS_SERVICE, CAP_DISPLAY | CAP_MMIO);
        }
        if (desk_h >= 0 && proc_wait(desk_h, &status, 5) == OK) {
            proc_close(desk_h);
            dprintf(2, "init: DESKTOP exited status=%d, restart\n", status);
            desk_h = spawn_service("SYS:/DESKTOP", desk_argv, desk_env,
                                   CLASS_SERVICE, 0);
        }
        if (dhcp_h >= 0 && proc_wait(dhcp_h, &status, 5) == OK) {
            proc_close(dhcp_h);
            dprintf(2, "init: DHCP exited status=%d, restart\n", status);
            dhcp_h = spawn_service("SYS:/DHCPCLIENT", dhcp_argv, NULL,
                                   CLASS_SERVICE, 0);
        }
        if (shell_h >= 0 && proc_wait(shell_h, &status, 5) == OK) {
            proc_close(shell_h);
            dprintf(2, "init: SHELL exited status=%d, restart\n", status);
            shell_h = spawn_service("SYS:/SHELL", shell_argv, shell_env,
                                    CLASS_USER, 0);
        }
        sys_sleep_ms(50);
    }
    return 0;
}
