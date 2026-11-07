#include "syscall.h"
#include "stdio.h"
#include "string.h"

int main(void) {
    printf("\033[2J\033[H");   /* 清屏 + 归位 */

    printf("\033[37mStandard white\033[0m\n");
    printf("\033[31mRed\033[0m  \033[32mGreen\033[0m  \033[34mBlue\033[0m\n");
    printf("\033[91mBright red\033[0m  \033[92mBright green\033[0m\n");
    printf("\033[44m  Blue background  \033[0m\n");
    printf("\033[41;37m  Red bg / white fg  \033[0m\n");
    printf("\033[7mReversed\033[0m\n");
    printf("\n");

    /* 光标定位 */
    printf("\033[10;5HCursor at (10,5)\033[0m");
    printf("\033[12;5HCursor at (12,5)\033[0m");

    /* 清行 */
    printf("\033[14;1H\033[41mXXXXXX\033[2KXX\033[0m");

    /* 光标移动 */
    printf("\033[16;1HLine 16 \033[3Cright\033[0m");
    printf("\033[18;1HLine 18 \033[3Dleft\033[0m");

    printf("\nDone.\n");
    return 0;
}