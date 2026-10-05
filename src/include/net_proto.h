#ifndef NET_PROTO_H
#define NET_PROTO_H

#include <stdint.h>

/* ---------- 字节序 ---------- */
static inline uint16_t htons(uint16_t x) {
    return ((x & 0xFF) << 8) | ((x >> 8) & 0xFF);
}
static inline uint16_t ntohs(uint16_t x) { return htons(x); }
static inline uint32_t htonl(uint32_t x) {
    return ((x & 0xFF) << 24) | ((x & 0xFF00) << 8) |
           ((x >> 8) & 0xFF00) | ((x >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t x) { return htonl(x); }

/* ---------- 本机配置（QEMU 用户模式默认） ----------
 * 宏是主机字节序；运行时 htonl 转网络字节序。 */
#define NET_IP_LOCAL_HOST      0x0A00020Fu   /* 10.0.2.15 */
#define NET_IP_GATEWAY_HOST    0x0A000202u   /* 10.0.2.2 */
#define NET_IP_DNS_HOST        0x0A000203u   /* 10.0.2.3 (QEMU slirp) */
#define NET_IP_NETMASK_HOST    0xFFFFFF00u
#define NET_IP_BROADCAST_HOST  0x0A0002FFu

/* ---------- Ethertype ---------- */
#define ETH_TYPE_IP   0x0800
#define ETH_TYPE_ARP  0x0806

/* ---------- IP 协议号 ---------- */
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

/* ---------- 本地状态 ---------- */
void net_set_local(const uint8_t *mac, uint32_t ip_be);
const uint8_t *net_local_mac(void);
uint32_t net_local_ip(void);
uint32_t net_local_netmask(void);

/* ---------- 初始化 ---------- */
void net_proto_init(void);

/* ---------- 帧入口 ---------- */
void net_proto_rx(const uint8_t *frame, uint32_t len);

/* ---------- 以太网 ---------- */
int net_eth_send(const uint8_t *dst_mac, uint16_t ethertype_be,
                 const uint8_t *payload, uint32_t len);

/* ---------- ARP ---------- */
void net_arp_init(void);
void net_arp_rx(const uint8_t *payload, uint32_t len);
int  net_arp_resolve(uint32_t ip_be, uint8_t *out_mac);
void net_arp_learn(uint32_t ip_be, const uint8_t *mac);

/* ---------- IPv4 ---------- */
void net_ip_rx(const uint8_t *p, uint32_t len, const uint8_t *src_mac);
int  net_ip_send(uint32_t dst_ip_be, uint8_t proto,
                 const uint8_t *payload, uint32_t len);

/* ---------- ICMP ---------- */
void net_icmp_rx(uint32_t src_ip_be, const uint8_t *p, uint32_t len);
int net_icmp_send(uint32_t dst_ip_be, const uint8_t *icmp_payload, uint32_t len);

/* 因是共享 API，公开 */
uint16_t ip_checksum(const uint8_t *data, uint32_t len);


#endif