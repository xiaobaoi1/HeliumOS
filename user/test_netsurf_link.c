#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "libwapcaplet/libwapcaplet.h"

int main(void) {
    printf("netsurf link test\n");

    lwc_string *s = NULL;
    if (lwc_intern_string("hello", 5, &s) != lwc_error_ok) {
        printf("lwc_intern FAIL\n");
        return 1;
    }
    printf("lwc len=%u\n", (unsigned)lwc_string_length(s));
    lwc_string_unref(s);

    printf("netsurf link OK\n");
    return 0;
}