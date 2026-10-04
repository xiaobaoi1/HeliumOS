/* user/test_kill.c
 * 测试 kill 路径。
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "errno.h"
#include "signal.h"

#define C_BLACK   0
#define C_GREEN   10
#define C_RED     12
#define C_YELLOW  14
#define C_BLUE    9
#define C_GRAY    7

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) {
    tty_set_color(C_GREEN, C_BLACK);
    printf("  [PASS] %s\n", name);
    tty_set_color(C_GRAY, C_BLACK);
    g_pass++;
}

static void fail(const char *name, const char *why) {
    tty_set_color(C_RED, C_BLACK);
    printf("  [FAIL] %s: %s\n", name, why);
    tty_set_color(C_GRAY, C_BLACK);
    g_fail++;
}

static void section(const char *title) {
    printf("\n");
    tty_set_color(C_BLUE, C_BLACK);
    printf("--- %s ---\n", title);
    tty_set_color(C_GRAY, C_BLACK);
}

static int spawn_sleeper(void) {
    int h = spawn("SLEEPER", NULL, NULL);
    if (h < 0) {
        tty_set_color(C_RED, C_BLACK);
        printf("  spawn SLEEPER failed: %d\n", h);
        tty_set_color(C_GRAY, C_BLACK);
    }
    return h;
}

/* ---------- T1: kill(正在 sleep) + wait ----------
 *
 * SIGTERM = 15 → 进 pending → sleeper 被唤醒 → sleep 返回 EINTR
 * → 无 handler（SIG_DFL）→ 终止，退出码 = 128 + 15 = 143
 */
static void test_kill_sleeping(void) {
    section("kill sleeping + wait (SIGTERM)");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER");

    sleep_ms(50);

    int r = kill(h, SIGTERM);
    if (r == OK) ok("kill(h, SIGTERM)");
    else         fail("kill", "returned error");

    int status = -1;
    int wr = wait(h, &status, 0);
    if (wr == OK && status == 143) {
        ok("wait returns status=143 (128+SIGTERM)");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "wr=%d status=%d (expect 143)", wr, status);
        fail("wait", buf);
    }

    int cr = process_close(h);
    if (cr == OK) ok("process_close");
    else          fail("process_close", "returned error");
}

/* ---------- T2: kill(SIGKILL) 后直接 close ---------- */

static void test_kill_then_close(void) {
    section("kill then close (no wait, SIGKILL)");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER");

    sleep_ms(50);

    if (kill(h, SIGKILL) == OK) ok("kill(h, SIGKILL)");
    else                        fail("kill", "returned error");

    if (process_close(h) == OK) ok("process_close without wait");
    else                        fail("process_close", "returned error");
}

/* ---------- T3: 连跑 20 轮 ----------
 *
 * SIGKILL = 9 → 立即终止，退出码 = 128 + 9 = 137
 */
static void test_stress(void) {
    section("stress: 20 rounds spawn/SIGKILL/wait/close");

    int failures = 0;
    for (int i = 0; i < 20; i++) {
        int h = spawn_sleeper();
        if (h < 0) { failures++; continue; }

        sleep_ms(50);
        if (kill(h, SIGKILL) != OK) { failures++; process_close(h); continue; }

        int status = -1;
        if (wait(h, &status, 0) != OK || status != 137) {
            failures++;
            process_close(h);
            continue;
        }
        process_close(h);
    }

    if (failures == 0) {
        ok("20 rounds all passed");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d of 20 failed", failures);
        fail("stress", buf);
    }
}

/* ---------- T4: wait 超时 → SIGKILL ---------- */

static void test_wait_timeout_then_kill(void) {
    section("wait timeout then SIGKILL");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER");

    int status = 0;
    int wr = wait(h, &status, 50);
    if (wr == EAGAIN) {
        ok("wait returns EAGAIN on timeout");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "wr=%d (expect %d)", wr, EAGAIN);
        fail("wait timeout", buf);
    }

    if (kill(h, SIGKILL) == OK) ok("kill(SIGKILL) after timeout");
    else                        fail("kill", "returned error");

    if (wait(h, &status, 100) == OK && status == 137) {
        ok("wait after SIGKILL returns 137");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "status=%d (expect 137)", status);
        fail("wait after kill", buf);
    }

    process_close(h);
}

/* ---------- 主入口 ---------- */

void main(int argc, char **argv) {
    (void)argc; (void)argv;

    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    printf("  HeliumOS Kill Test\n");
    printf("========================================\n");
    tty_set_color(C_GRAY, C_BLACK);

    test_kill_sleeping();
    test_kill_then_close();
    test_stress();
    test_wait_timeout_then_kill();

    printf("\n");
    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    if (g_fail == 0) {
        tty_set_color(C_GREEN, C_BLACK);
        printf("  ALL PASSED (%d checks)\n", g_pass);
    } else {
        tty_set_color(C_RED, C_BLACK);
        printf("  %d PASSED, %d FAILED\n", g_pass, g_fail);
    }
    tty_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    tty_set_color(C_GRAY, C_BLACK);

    return g_fail;
}