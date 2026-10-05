#ifndef USER_ARPA_INET_H
#define USER_ARPA_INET_H

#include <stdint.h>

uint32_t inet_addr(const char *cp);
char    *inet_ntoa(uint32_t in);

#endif