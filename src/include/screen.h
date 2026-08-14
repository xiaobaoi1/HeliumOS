#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>

void screen_init(void);
void screen_write_string(const char *str);
void screen_write_char(char c);

#endif