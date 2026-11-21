#ifndef NET_SOCK_H
#define NET_SOCK_H

#include <stdint.h>

/* 前向声明 */
struct task;   
struct tcp_socket;

/* ---------- 每进程句柄 ---------- */
#define SOCK_MAX_HANDLES 8
#define SOCK_STREAM 1

/* ---------- 全局 UDP socket 池 ---------- */
#define UDP_MAX_SOCKETS   16
#define UDP_RX_QUEUE_LEN  8
#define UDP_PAYLOAD_MAX   1472

/* ---------- 常量（与用户态一致） ---------- */
#define AF_INET     2
#define SOCK_DGRAM  2
#define SOCK_ICMP   3


/* ---------- 结构 ---------- */

struct udp_packet {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[UDP_PAYLOAD_MAX];
};

struct udp_socket {
    uint8_t  used;
    uint16_t local_port;
    uint8_t  reserved;
    uint32_t refcount;
    struct udp_packet *rx_queue[UDP_RX_QUEUE_LEN];
    uint8_t  rx_head;
    uint8_t  rx_tail;
    uint8_t  rx_count;
    uint8_t  pad;
};

struct sock_handle {
    uint8_t  used;
    uint8_t  type;    /* SOCK_DGRAM */
    uint16_t reserved;
    struct udp_socket *sock;
    struct tcp_socket *tcp;
};

/* ---------- 内核镜像：sockaddr_in ---------- */
struct sockaddr_in {
    uint16_t sin_family;    /* AF_INET */
    uint16_t sin_port;      /* 网络字节序 */
    uint32_t sin_addr;      /* 网络字节序 */
    uint8_t  sin_zero[8];
} __attribute__((packed));

/* ---------- 生命周期 ---------- */
void net_sock_init(void);
void net_sock_release_all(struct task *t);

/* ---------- 内核 API ---------- */
struct udp_socket *udp_socket_alloc(void);
void               udp_socket_free(struct udp_socket *s);
struct udp_socket *udp_socket_by_port(uint16_t port_be);

/* ---------- UDP 协议 ---------- */
void net_udp_rx(uint32_t src_ip, uint16_t src_port,
                uint16_t dst_port, const uint8_t *payload, uint32_t len);
int  net_udp_send(uint32_t dst_ip_be, uint16_t src_port_be,
                  uint16_t dst_port_be, const uint8_t *data, uint32_t len);

/* ---------- syscall 实现 ---------- */
int sys_socket(int domain, int type, int proto);
int sys_bind(int h, const void *addr_user, uint32_t addrlen);
int sys_sendto(int h, const void *buf_user, uint32_t len,
               const void *dst_user, uint32_t addrlen);
int sys_recvfrom(int h, void *buf_user, uint32_t max,
                 void *src_user, uint32_t *addrlen_user);
int sys_sockclose(int h);
int sys_connect(int h, const void *addr_user, uint32_t addrlen);
int sys_listen(int h, int backlog);
int sys_accept(int h, void *addr_user, uint32_t *addrlen_user);

#endif