#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

#include <stdint.h>
#include "syscall.h"

/* 与内核 signal.h 一致 */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20

#define SIG_DFL     0
#define SIG_IGN     1

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

struct sigaction {
    uint32_t sa_handler;
    uint32_t sa_mask;
    uint32_t sa_flags;
    uint32_t sa_restorer;
};

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigprocmask(int how, const uint32_t *set, uint32_t *oldset);

static inline int raise(int sig) {
    return __syscall(SYS_KILL, -1, sig, 0);
}

#endif