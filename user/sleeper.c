/* user/sleeper.c
 * 一个只会 sleep 的进程，供 test_kill.c 当靶子。
 * 被 kill 时必定处于 SLEEPING 状态（在 blocked_list 里）。
 */

#include "syscall.h"
#include "stdio.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("[sleeper] pid=%d, entering long sleep\n", getpid());
    for (;;) {
        sleep_ms(100000);
    }
    return 0;
}