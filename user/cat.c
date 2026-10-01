#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "signal.h"
#include "stdlib.h"

#define VGA_BLACK        0
#define VGA_LIGHT_GRAY   7
#define VGA_LIGHT_BLUE   9
#define VGA_LIGHT_GREEN  10
#define VGA_LIGHT_RED    12

#define CMD_MAX          128
#define ARGV_MAX         8
#define CAT_BUF_SIZE     512

static void set_color(int fg, int bg) {
    tty_set_color(fg, bg);
}

int main(int argc, char **argv){
    (void)argc; (void)argv;
    if (argc < 2) {
        printf("usage: cat <file>\n");
        return;
    }

    int fd = fs_open(argv[1], FS_O_RDONLY);
    if (fd < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("cat: %s: error %d\n", argv[1], fd);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
        return;
    }

    char buf[CAT_BUF_SIZE];
    int n;
    while ((n = fs_read(fd, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }
    fs_close(fd);

    if (n < 0) {
        set_color(VGA_LIGHT_RED, VGA_BLACK);
        printf("\ncat: read error %d\n", n);
        set_color(VGA_LIGHT_GRAY, VGA_BLACK);
    }
}