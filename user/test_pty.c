#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "signal.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int fds[2];
    if (pty_open(fds) < 0) { printf("pty_open failed\n"); return 1; }

    int master = fds[0];
    int slave  = fds[1];
    printf("pty: master=%d slave=%d\n", master, slave);

    /* 测试 1：通过 pty 跑 COUNT，用 ^D 给它 EOF */
    {
        struct spawn_params p = {
            .size = sizeof(p),
            .in_fd = SPAWN_FD_INHERIT,
            .out_fd = SPAWN_FD_INHERIT,
            .err_fd = SPAWN_FD_INHERIT,
            .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
            .in_pty = slave, .out_pty = slave, .err_pty = slave,
            .flags = 0,
            .envp = 0,
            .out_pid = 0,
        };
        int h = spawn("COUNT", NULL, &p);
        if (h < 0) { printf("spawn: %d\n", h); return 1; }
        printf("spawned COUNT handle=%d pid=%u\n", h, p.out_pid);

        pty_set_fg(master, p.out_pid);

        const char *msg = "hello pty!\n";
        pty_write(master, msg, strlen(msg));

        sleep_ms(100);

        char eof = 0x04;
        pty_write(master, &eof, 1);
        sleep_ms(100);

        char buf[128];
        int n = pty_read(master, buf, sizeof(buf));
        if (n > 0) {
            printf("master got %d bytes: ", n);
            for (int i = 0; i < n; i++) putchar(buf[i]);
            printf("\n");
        }

        int st;
        wait(h, &st, 1000);
        process_close(h);
    }

    /* 测试 2：^C 杀死前台 */
    {
        struct spawn_params p = {
            .size = sizeof(p),
            .in_fd = SPAWN_FD_INHERIT,
            .out_fd = SPAWN_FD_INHERIT,
            .err_fd = SPAWN_FD_INHERIT,
            .in_pipe = -1, .out_pipe = -1, .err_pipe = -1,
            .in_pty = slave, .out_pty = slave, .err_pty = slave,
            .flags = 0, .envp = 0, .out_pid = 0,
        };
        int h = spawn("SLEEPER", NULL, &p);
        if (h < 0) { printf("spawn 2: %d\n", h); return 1; }
        printf("spawned SLEEPER handle=%d pid=%u\n", h, p.out_pid);

        pty_set_fg(master, p.out_pid);
        sleep_ms(50);

        char ctrl_c = 0x03;
        pty_write(master, &ctrl_c, 1);
        sleep_ms(100);

        int st;
        int wr = wait(h, &st, 500);
        if (wr == 0 && st == 130) {
            printf("^C test: PASS (status 130)\n");
        } else {
            printf("^C test: FAIL (wr=%d st=%d)\n", wr, st);
        }
        process_close(h);
    }

    pty_close(master);
    pty_close(slave);
    printf("done\n");
    return 0;
}