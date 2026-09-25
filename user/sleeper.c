/* user/sleeper.c
 * 一个只会 sleep 的进程，供 test_kill.c 当靶子。
 * 被 kill 时必定处于 SLEEPING 状态（在 blocked_list 里）。
 */

#include "syscall.h"
#include "stdio.h"

void _start(void) {
    printf("[sleeper] pid=%d, entering long sleep\n", getpid());
    /* 睡 100 秒，等被杀 */
    for (;;) {
        sleep_ms(100000);
    }
    _exit(0);   /* 不会到达 */
}