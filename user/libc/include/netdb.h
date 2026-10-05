#ifndef USER_NETDB_H
#define USER_NETDB_H

struct hostent {
    char  *h_name;
    char **h_aliases;
    int    h_addrtype;
    int    h_length;
    char **h_addr_list;   /* 指向 in_addr（网络字节序）数组 */
};

struct hostent *gethostbyname(const char *name);

/* 简化的 DNS 配置——固定 QEMU slirp 的 DNS 服务器 */
#define DNS_SERVER_IP  0x0302000Au   /* 10.0.2.3 网络字节序 */

#endif