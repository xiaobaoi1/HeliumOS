#ifndef TCP_H
#define TCP_H

#include <stdint.h>

#define TCP_MAX_SOCKETS   16
#define TCP_RX_BUF_SIZE   4096
#define TCP_MSS           1460

/* 状态 */
#define TCP_CLOSED       0
#define TCP_LISTEN       1
#define TCP_SYN_SENT     2
#define TCP_SYN_RCVD     3
#define TCP_ESTABLISHED  4
#define TCP_FIN_WAIT_1   5
#define TCP_FIN_WAIT_2   6
#define TCP_CLOSE_WAIT   7
#define TCP_CLOSING      8
#define TCP_LAST_ACK     9
#define TCP_TIME_WAIT    10

/* flags */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20


#define TCP_BACKLOG   8

/* TCP 头（不含 options） */
struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_off;   /* 高 4 位 = 头部长度 / 4 */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

struct tcp_socket {
    uint8_t  used;
    uint8_t  state;
    uint16_t local_port;
    uint32_t remote_ip;      /* 网络字节序 */
    uint16_t remote_port;    /* 网络字节序 */
    uint16_t reserved;
    uint8_t  user_closed;    /* 用户已调 close；TCP 状态机走完后回收 */
    uint8_t  pad[3];

    /* LISTEN 状态用：accept 队列 */
    struct tcp_socket *accept_queue[TCP_BACKLOG];
    int accept_count;

    /* 序列号 */
    uint32_t iss;        /* 初始发送 seq */
    uint32_t snd_una;    /* 已发送未确认 */
    uint32_t snd_nxt;    /* 下一个要发的 seq */
    uint32_t rcv_nxt;    /* 下一个要接收的 seq */

    /* 接收缓冲（C.2.4 用） */
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t rx_count;
};

void tcp_init(void);

/* 池管理 */
struct tcp_socket *tcp_alloc(void);
void tcp_free(struct tcp_socket *s);
struct tcp_socket *tcp_find_by_local_port(uint16_t local_port_be);

/* 主动/被动打开 */
int tcp_listen(struct tcp_socket *s, uint16_t local_port_be);
int tcp_connect(struct tcp_socket *s, uint32_t remote_ip, uint16_t remote_port_be);
struct tcp_socket *tcp_accept(struct tcp_socket *listen);

/* 收发 */
int tcp_send(struct tcp_socket *s, const uint8_t *data, uint32_t len);
int tcp_recv(struct tcp_socket *s, uint8_t *buf, uint32_t max);

/* 由 net_proto.c 的 net_ip_rx 调用 */
void tcp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t *pkt, uint32_t len);

/* 伪头 + TCP 段的 16 位校验和 */
uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                      const uint8_t *tcp_seg, uint32_t len);
                    
int tcp_close(struct tcp_socket *s);

#endif