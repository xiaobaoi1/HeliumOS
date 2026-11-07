#include "signal.h"
#include "syscall.h"

extern void __sigreturn_trampoline(void);

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) {
    struct sigaction new_act;
    const void *new_p = 0;
    void *old_p = 0;

    if (act) {
        new_act = *act;
        if (new_act.sa_handler != SIG_DFL && new_act.sa_handler != SIG_IGN) {
            new_act.sa_restorer = (uint32_t)__sigreturn_trampoline;
        }
        new_p = &new_act;
    }
    if (oldact) old_p = oldact;

    return __syscall(SYS_SIGACTION, sig, (int)new_p, (int)old_p);
}

int sigprocmask(int how, const uint32_t *set, uint32_t *oldset) {
    return __syscall(SYS_SIGPROCMASK, how, (int)set, (int)oldset);
}