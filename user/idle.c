#include "syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (;;) {
        sys_sleep_ms(1000);
    }
    return 0;
}