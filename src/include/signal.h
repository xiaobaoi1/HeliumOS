#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

/* ---------- 信号编号（标准 POSIX i386） ---------- */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9    /* 不可捕获/阻塞/忽略 */
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19   /* 不可捕获/阻塞/忽略 */
#define SIGTSTP     20

/* ---------- 特殊 handler 值 ---------- */
#define SIG_DFL     0
#define SIG_IGN     1

/* ---------- sigprocmask 的 how ---------- */
#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

/* ---------- 内核态 sigaction ---------- */
struct sig_action {
    uint32_t handler;
    uint32_t mask;
    uint32_t flags;
    uint32_t restorer;
};

/* 前向声明 */
struct task;
struct registers;

/* ---------- 核心 API ---------- */

/* 从内核态返回用户态前投递 pending signals。
 * 可能：
 *   - 投递用户 handler（改 regs->eip / user_esp）
 *   - 默认行为终止进程（调 task_terminate + schedule，不返回）
 *   - 无 pending 或全被屏蔽 → 立即返回 */
void signal_deliver_pending(struct registers *regs, struct task *cur);

/* 用户异常 int_no → 信号编号 */
int signal_from_exception(int int_no);

/* 给键盘前台进程发信号（Ctrl+C 用） */
void signal_foreground(int sig);

/* ---------- 系统调用实现 ---------- */
int  sys_sigaction(int sig, void *new_user, void *old_user);
int  sys_sigprocmask(int how, uint32_t *set_user, uint32_t *old_user);
void sys_sigreturn(struct registers *regs);

#endif