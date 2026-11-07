#include "syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    for (;;) {
        sleep_ms(1000);
    }
    return 0;
}