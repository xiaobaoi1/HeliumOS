// user/test_raw.c
#include "syscall.h"
#include "stdio.h"
#include <termios.h>
#include <sys/ioctl.h>

int main(void) {
    // struct winsize ws;
    // if (ioctl(1, TIOCGWINSZ, &ws) == 0) {
    //     printf("winsize: %dx%d\n", ws.ws_col, ws.ws_row);
    // } else {
    //     printf("ioctl failed\n");
    // }

    // struct termios orig, raw;
    // tcgetattr(0, &orig);
    // raw = orig;
    // raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    // tcsetattr(0, TCSANOW, &raw);

    // printf("raw mode: type 5 chars\n");
    // for (int i = 0; i < 5; i++) {
    //     char c;
    //     read(0, &c, 1);
    //     printf("[%d]\n", (int)(unsigned char)c);
    // }

    // tcsetattr(0, TCSANOW, &orig);
    // printf("cooked mode restored\n");
    // return 0;
}