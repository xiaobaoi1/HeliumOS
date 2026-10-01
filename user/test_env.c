#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("PATH     = %s\n", getenv("PATH"));
    printf("HOME     = %s\n", getenv("HOME"));
    printf("TERM     = %s\n", getenv("TERM"));
    printf("NONEXIST = %s\n", getenv("NONEXIST"));

    setenv("MYVAR", "hello", 1);
    printf("MYVAR    = %s\n", getenv("MYVAR"));

    setenv("MYVAR", "world", 1);
    printf("MYVAR    = %s (after overwrite)\n", getenv("MYVAR"));

    unsetenv("MYVAR");
    printf("MYVAR    = %s (after unset)\n", getenv("MYVAR"));

    return 0;
}