#ifndef USER_NETINET_IN_H
#define USER_NETINET_IN_H

#include <sys/socket.h>
#include <arpa/inet.h>

typedef unsigned int   in_addr_t;
typedef unsigned short in_port_t;

#ifndef INADDR_ANY
#define INADDR_ANY ((in_addr_t)0x00000000)
#endif

struct in6_addr {
    unsigned char s6_addr[16];
};

#endif