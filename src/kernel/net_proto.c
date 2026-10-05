#include <net_proto.h>
#include <net.h>
#include <heap.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

/* ---------- 本地状态 ---------- */

static uint8_t  g_mac[6];
static uint32_t g_ip       = 0;   /* 网络字节序 */
static uint32_t g_netmask  = 0;
static uint32_t g_gateway  = 0;
static uint32_t g_broadcast = 0;

/* ---------- ARP 缓存 ---------- */

#define ARP_CACHE_SIZE 8
struct arp_entry {
    uint32_t ip_be;
    uint8_t  mac[6];
    uint8_t  valid;
    uint8_t  pad;
};
static struct arp_entry g_arp[ARP_CACHE_SIZE];

/* ---------- 校验和 ---------- */

static uint16_t ip_checksum(const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)data[i] << 8) | data[i + 1];
    }
    if (len & 1) sum += (uint16_t)data[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---------- 本地配置访问 ---------- */

void net_set_local(const uint8_t *mac, uint32_t ip_be) {
    if (mac) memcpy(g_mac, mac, 6);
    if (ip_be) g_ip = ip_be;
}

const uint8_t *net_local_mac(void) { return g_mac; }
uint32_t net_local_ip(void) { return g_ip; }
uint32_t net_local_netmask(void) { return g_netmask; }

/* ---------- 以太网发送 ---------- */

int net_eth_send(const uint8_t *dst_mac, uint16_t ethertype,
                 const uint8_t *payload, uint32_t len) {
    if (len > 1500) return -1;

    uint8_t *frame = (uint8_t*)kmalloc(14 + len);
    if (!frame) return -1;

    memcpy(frame, dst_mac, 6);
    memcpy(frame + 6, g_mac, 6);
    frame[12] = (ethertype >> 8) & 0xFF;
    frame[13] = ethertype & 0xFF;
    memcpy(frame + 14, payload, len);

    uint32_t total = 14 + len;

    /* 最小帧长 60（不含 FCS）。kmalloc 是 slab，不能 memset 到边界外，
     * 所以直接发实际长度——驱动层如需补零会自己处理。 */
    int r = net_send("eth0", frame, total);
    kfree(frame);
    return r;
}

/* ---------- 以太网接收 ---------- */

static void net_eth_rx(const uint8_t *frame, uint32_t len) {
    if (len < 14) return;

    uint16_t ethertype = ((uint16_t)frame[12] << 8) | frame[13];
    const uint8_t *src_mac = frame + 6;
    const uint8_t *payload = frame + 14;
    uint32_t payload_len = len - 14;

    if (ethertype == ETH_TYPE_ARP) {
        net_arp_rx(payload, payload_len);
    } else if (ethertype == ETH_TYPE_IP) {
        net_ip_rx(payload, payload_len, src_mac);
    }
}

/* ---------- ARP ---------- */

void net_arp_init(void) {
    memset(g_arp, 0, sizeof(g_arp));
}

void net_arp_learn(uint32_t ip_be, const uint8_t *mac) {
    /* 先找已存在的 */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip_be == ip_be) {
            memcpy(g_arp[i].mac, mac, 6);
            return;
        }
    }
    /* 找空槽 */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!g_arp[i].valid) {
            g_arp[i].ip_be = ip_be;
            memcpy(g_arp[i].mac, mac, 6);
            g_arp[i].valid = 1;
            return;
        }
    }
    /* 满了——覆盖第 0 项（简单策略） */
    g_arp[0].ip_be = ip_be;
    memcpy(g_arp[0].mac, mac, 6);
    g_arp[0].valid = 1;
}

void net_arp_rx(const uint8_t *p, uint32_t len) {
    if (len < 28) return;

    uint16_t op = ((uint16_t)p[6] << 8) | p[7];
    const uint8_t *sender_mac = p + 8;
    uint32_t sender_ip;
    memcpy(&sender_ip, p + 14, 4);
    uint32_t target_ip;
    memcpy(&target_ip, p + 24, 4);

    net_arp_learn(sender_ip, sender_mac);

    /* 对我们的请求 → 回响应 */
    if (op == 1 && target_ip == g_ip) {
        uint8_t reply[28];
        memset(reply, 0, sizeof(reply));
        reply[0] = 0; reply[1] = 1;         /* htype = Ethernet */
        reply[2] = 0x08; reply[3] = 0x00;   /* ptype = IPv4 */
        reply[4] = 6; reply[5] = 4;         /* hlen / plen */
        reply[6] = 0; reply[7] = 2;         /* op = reply */
        memcpy(reply + 8, g_mac, 6);
        memcpy(reply + 14, &g_ip, 4);
        memcpy(reply + 18, sender_mac, 6);
        memcpy(reply + 24, &sender_ip, 4);

        net_eth_send(sender_mac, ETH_TYPE_ARP, reply, 28);
        kprintf("[ARP] Replied to %u.%u.%u.%u\n",
                ntohl(sender_ip) >> 24, (ntohl(sender_ip) >> 16) & 0xFF,
                (ntohl(sender_ip) >> 8) & 0xFF, ntohl(sender_ip) & 0xFF);
    }
}

int net_arp_resolve(uint32_t ip_be, uint8_t *out_mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip_be == ip_be) {
            memcpy(out_mac, g_arp[i].mac, 6);
            return 0;
        }
    }
    return -1;
}

/* ---------- IPv4 接收 ---------- */

void net_ip_rx(const uint8_t *p, uint32_t len, const uint8_t *src_mac) {
    if (len < 20) return;

    uint8_t ver_ihl = p[0];
    uint8_t version = ver_ihl >> 4;
    uint8_t ihl = (ver_ihl & 0x0F) * 4;
    if (version != 4 || ihl < 20 || ihl > len) return;

    uint16_t total_len = ((uint16_t)p[2] << 8) | p[3];
    if (total_len < ihl || total_len > len) return;

    uint32_t src_ip;
    memcpy(&src_ip, p + 12, 4);
    uint32_t dst_ip;
    memcpy(&dst_ip, p + 16, 4);

    /* 从 IP 包学到 ARP 映射 */
    net_arp_learn(src_ip, src_mac);

    if (dst_ip != g_ip && dst_ip != g_broadcast) return;

    uint8_t proto = p[9];
    const uint8_t *payload = p + ihl;
    uint32_t payload_len = total_len - ihl;

    if (proto == IP_PROTO_ICMP) {
        net_icmp_rx(src_ip, payload, payload_len);
    }
    /* UDP 在 B4b */
}

/* ---------- IPv4 发送 ---------- */

int net_ip_send(uint32_t dst_ip_be, uint8_t proto,
                const uint8_t *payload, uint32_t len) {
    if (len > 1480) return -1;

    /* 路由：同网段直连，否则走网关 */
    uint32_t next_hop = dst_ip_be;
    if ((dst_ip_be & g_netmask) != (g_ip & g_netmask)) {
        next_hop = g_gateway;
    }

    uint8_t dst_mac[6];
    if (net_arp_resolve(next_hop, dst_mac) < 0) {
        /* ARP 缓存未命中：发请求，返回 EAGAIN */
        uint8_t req[28];
        memset(req, 0, sizeof(req));
        req[0] = 0; req[1] = 1;
        req[2] = 0x08; req[3] = 0x00;
        req[4] = 6; req[5] = 4;
        req[6] = 0; req[7] = 1;
        memcpy(req + 8, g_mac, 6);
        memcpy(req + 14, &g_ip, 4);
        memcpy(req + 24, &next_hop, 4);

        uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        net_eth_send(bcast, ETH_TYPE_ARP, req, 28);
        kprintf("[IP] ARP miss for next hop\n");
        return -1;
    }

    uint8_t *pkt = (uint8_t*)kmalloc(20 + len);
    if (!pkt) return -1;

    memset(pkt, 0, 20);
    pkt[0] = 0x45;                  /* version=4, IHL=5 */
    uint16_t tot = (uint16_t)(20 + len);
    pkt[2] = (tot >> 8) & 0xFF;
    pkt[3] = tot & 0xFF;
    pkt[6] = 0x40;                  /* flags = DF */
    pkt[8] = 64;                    /* TTL */
    pkt[9] = proto;
    memcpy(pkt + 12, &g_ip, 4);
    memcpy(pkt + 16, &dst_ip_be, 4);

    uint16_t csum = ip_checksum(pkt, 20);
    pkt[10] = (csum >> 8) & 0xFF;
    pkt[11] = csum & 0xFF;

    memcpy(pkt + 20, payload, len);

    int r = net_eth_send(dst_mac, ETH_TYPE_IP, pkt, 20 + len);
    kfree(pkt);
    return r;
}

/* ---------- ICMP ---------- */

void net_icmp_rx(uint32_t src_ip_be, const uint8_t *p, uint32_t len) {
    if (len < 8) return;

    uint8_t type = p[0];
    uint8_t code = p[1];

    /* Echo reply：打印，交给上层（未来） */
    if (type == 0 && code == 0) {
        uint32_t ip_h = ntohl(src_ip_be);
        kprintf("[ICMP] Echo reply from %u.%u.%u.%u\n",
                ip_h >> 24, (ip_h >> 16) & 0xFF,
                (ip_h >> 8) & 0xFF, ip_h & 0xFF);
        return;
    }

    /* Echo request：回 reply */
    if (type != 8 || code != 0) return;

    uint8_t *reply = (uint8_t*)kmalloc(len);
    if (!reply) return;

    memcpy(reply, p, len);
    reply[0] = 0;
    reply[2] = 0; reply[3] = 0;   /* checksum 先清 */

    uint16_t csum = ip_checksum(reply, len);
    reply[2] = (csum >> 8) & 0xFF;
    reply[3] = csum & 0xFF;

    net_ip_send(src_ip_be, IP_PROTO_ICMP, reply, len);
    kfree(reply);

    uint32_t ip_h = ntohl(src_ip_be);
    kprintf("[ICMP] Echo reply to %u.%u.%u.%u (%u bytes)\n",
            ip_h >> 24, (ip_h >> 16) & 0xFF,
            (ip_h >> 8) & 0xFF, ip_h & 0xFF, len);
}

/* ---------- 初始化 + 入口 ---------- */

void net_proto_init(void) {
    net_arp_init();

    g_ip        = htonl(NET_IP_LOCAL_HOST);
    g_netmask   = htonl(NET_IP_NETMASK_HOST);
    g_gateway   = htonl(NET_IP_GATEWAY_HOST);
    g_broadcast = htonl(NET_IP_BROADCAST_HOST);

    /* 从 net 子系统获取本机 MAC（由驱动注册） */
    struct net_device *nd = net_get("eth0");
    if (nd) net_set_local(nd->mac, g_ip);

    kprintf("[NET] Protocol stack ready: ip=10.0.2.15\n");
}

void net_proto_rx(const uint8_t *frame, uint32_t len) {
    net_eth_rx(frame, len);
}