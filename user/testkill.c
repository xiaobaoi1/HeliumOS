/* user/test_kill.c
 * 测试 kill 路径：
 *   T1: spawn + kill(正在 sleep) + wait + close   → 走 sys_kill 的完整路径
 *   T2: spawn + kill + close（不 wait）            → 检查 refcount 是否正常
 *   T3: 连续 20 轮，观察是否稳定（内存不泄漏、不崩）
 *   T4: kill 后 wait 立即返回（不等超时）
 */

#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "errno.h"

#define C_BLACK   0
#define C_GREEN   10
#define C_RED     12
#define C_YELLOW  14
#define C_BLUE    9
#define C_GRAY    7

static int g_pass = 0;
static int g_fail = 0;

static void ok(const char *name) {
    console_set_color(C_GREEN, C_BLACK);
    printf("  [PASS] %s\n", name);
    console_set_color(C_GRAY, C_BLACK);
    g_pass++;
}

static void fail(const char *name, const char *why) {
    console_set_color(C_RED, C_BLACK);
    printf("  [FAIL] %s: %s\n", name, why);
    console_set_color(C_GRAY, C_BLACK);
    g_fail++;
}

static void section(const char *title) {
    printf("\n");
    console_set_color(C_BLUE, C_BLACK);
    printf("--- %s ---\n", title);
    console_set_color(C_GRAY, C_BLACK);
}

/* 生成一个 "PID" 用于 log，不通过内核 API */
static int spawn_sleeper(void) {
    int h = spawn("SLEEPER.ELF", NULL);
    if (h < 0) {
        console_set_color(C_RED, C_BLACK);
        printf("  spawn SLEEPER.ELF failed: %d\n", h);
        console_set_color(C_GRAY, C_BLACK);
    }
    return h;
}

/* ---------- T1: kill 一个正在 sleep 的进程 + wait ---------- */

static void test_kill_sleeping(void) {
    section("kill sleeping + wait");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER.ELF");

    /* 让 sleeper 进入 sleep 状态（肯定在 blocked_list 里） */
    sleep_ms(50);

    int r = kill(h, 42);
    if (r == OK) ok("kill(h, 42)");
    else         fail("kill", "returned error");

    /* wait 应该立即返回（进程已是僵尸） */
    int status = -1;
    int wr = wait(h, &status, 0);
    if (wr == OK && status == 42) {
        ok("wait returns exit_status=42");
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "wr=%d status=%d", wr, status);
        fail("wait", buf);
    }

    int cr = process_close(h);
    if (cr == OK) ok("process_close");
    else          fail("process_close", "returned error");
}

/* ---------- T2: kill 后直接 close（不 wait） ---------- */

static void test_kill_then_close(void) {
    section("kill then close (no wait)");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER.ELF");

    sleep_ms(50);

    if (kill(h, 99) == OK) ok("kill(h, 99)");
    else                    fail("kill", "returned error");

    /* 不 wait，直接 close —— refcount 应该递减到 0，PCB 释放 */
    if (process_close(h) == OK) ok("process_close without wait");
    else                         fail("process_close", "returned error");
}

/* ---------- T3: 连跑 20 轮 ---------- */

static void test_stress(void) {
    section("stress: 20 rounds of spawn/kill/wait/close");

    int failures = 0;
    for (int i = 0; i < 20; i++) {
        int h = spawn_sleeper();
        if (h < 0) { failures++; continue; }

        sleep_ms(10);
        if (kill(h, i) != OK) { failures++; process_close(h); continue; }

        int status = -1;
        if (wait(h, &status, 0) != OK || status != i) {
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

/* ---------- T4: wait 超时后进程仍在跑，再 kill ---------- */

static void test_wait_timeout_then_kill(void) {
    section("wait timeout then kill");

    int h = spawn_sleeper();
    if (h < 0) { fail("spawn", "returned error"); return; }
    ok("spawn SLEEPER.ELF");

    /* 不给它时间 sleep，直接 wait(50ms) —— 应该超时 */
    int status = 0;
    int wr = wait(h, &status, 50);
    if (wr == EAGAIN) {
        ok("wait returns EAGAIN on timeout");
    } else {
        char buf[48];
        snprintf(buf, sizeof(buf), "wr=%d (expect %d)", wr, EAGAIN);
        fail("wait timeout", buf);
    }

    /* 现在 kill 它 */
    if (kill(h, 7) == OK) ok("kill after timeout");
    else                   fail("kill", "returned error");

    /* 再 wait 应该立即成功 */
    if (wait(h, &status, 100) == OK && status == 7) {
        ok("wait after kill returns status=7");
    } else {
        fail("wait after kill", "wrong result");
    }

    process_close(h);
}

/* ---------- 主入口 ---------- */

void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    console_clear();

    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    printf("  HeliumOS Kill Test\n");
    printf("========================================\n");
    console_set_color(C_GRAY, C_BLACK);

    test_kill_sleeping();
    test_kill_then_close();
    test_stress();
    test_wait_timeout_then_kill();

    printf("\n");
    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    if (g_fail == 0) {
        console_set_color(C_GREEN, C_BLACK);
        printf("  ALL PASSED (%d checks)\n", g_pass);
    } else {
        console_set_color(C_RED, C_BLACK);
        printf("  %d PASSED, %d FAILED\n", g_pass, g_fail);
    }
    console_set_color(C_YELLOW, C_BLACK);
    printf("========================================\n");
    console_set_color(C_GRAY, C_BLACK);

    return g_fail;
}