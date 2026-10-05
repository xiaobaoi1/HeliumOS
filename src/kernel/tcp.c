#include <tcp.h>
#include <net_proto.h>
#include <printf.h>
#include <string.h>
#include <stddef.h>

static struct tcp_socket g_tcp[TCP_MAX_SOCKETS];

void tcp_init(void) {
    memset(g_tcp, 0, sizeof(g_tcp));
}

/* 状态机走到 CLOSED 时调用。若用户已 close 且已无人引用，则回收。 */
static void tcp_maybe_free(struct tcp_socket *s) {
    if (!s) return;
    if (s->state != TCP_CLOSED) return;
    if (!s->user_closed) return;      /* 用户还在，等 close */
    tcp_free(s);
}

/* ---------- 池管理 ---------- */

struct tcp_socket *tcp_alloc(void) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (!g_tcp[i].used) {
            memset(&g_tcp[i], 0, sizeof(g_tcp[i]));
            g_tcp[i].used = 1;
            g_tcp[i].state = TCP_CLOSED;
            return &g_tcp[i];
        }
    }
    return NULL;
}

void tcp_free(struct tcp_socket *s) {
    if (!s) return;
    s->used = 0;
}

struct tcp_socket *tcp_find_by_local_port(uint16_t local_port_be) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        if (g_tcp[i].used && g_tcp[i].local_port == local_port_be) {
            return &g_tcp[i];
        }
    }
    return NULL;
}

/* 4 元组匹配：查找已建立/半连接状态下的连接 */
static struct tcp_socket *tcp_find_conn(uint32_t remote_ip,
                                         uint16_t remote_port_be,
                                         uint16_t local_port_be) {
    for (int i = 0; i < TCP_MAX_SOCKETS; i++) {
        struct tcp_socket *s = &g_tcp[i];
        if (!s->used) continue;
        if (s->state == TCP_LISTEN || s->state == TCP_CLOSED) continue;
        if (s->remote_ip == remote_ip &&
            s->remote_port == remote_port_be &&
            s->local_port == local_port_be) {
            return s;
        }
    }
    return NULL;
}

/* ---------- 校验和 ---------- */

uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                      const uint8_t *tcp_seg, uint32_t len) {
    uint32_t sum = 0;

    const uint8_t *s = (const uint8_t*)&src_ip;
    const uint8_t *d = (const uint8_t*)&dst_ip;
    sum += ((uint16_t)s[0] << 8) | s[1];
    sum += ((uint16_t)s[2] << 8) | s[3];
    sum += ((uint16_t)d[0] << 8) | d[1];
    sum += ((uint16_t)d[2] << 8) | d[3];
    sum += 0x0006;
    sum += len & 0xFFFF;

    for (uint32_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)tcp_seg[i] << 8) | tcp_seg[i + 1];
    }
    if (len & 1) sum += (uint16_t)tcp_seg[len - 1] << 8;

    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)(~sum);
}

/* ---------- 发送 ---------- */

/* 发送一个 TCP 段。payload 可为 NULL（len=0）。 */
static int tcp_send_segment(struct tcp_socket *s, uint32_t src_ip,
                            uint8_t flags, uint32_t seq, uint32_t ack,
                            const uint8_t *payload, uint32_t payload_len) {
    /* SYN 段带 4 字节 MSS option；其他段不带 */
    int has_mss = (flags & TCP_SYN) ? 1 : 0;
    uint32_t hdr_len = has_mss ? 24 : 20;
    uint32_t total = hdr_len + payload_len;

    uint8_t *pkt = (uint8_t*)kmalloc(total);
    if (!pkt) return -1;
    memset(pkt, 0, total);

    struct tcp_hdr *h = (struct tcp_hdr*)pkt;
    h->src_port = s->local_port;
    h->dst_port = s->remote_port;
    h->seq      = htonl(seq);
    h->ack      = htonl(ack);
    h->data_off = (uint8_t)((hdr_len / 4) << 4);
    h->flags    = flags;
    h->window   = htons(4096);
    h->checksum = 0;
    h->urgent   = 0;

    if (has_mss) {
        /* MSS option: kind=2, len=4, mss=1460 */
        pkt[20] = 2;
        pkt[21] = 4;
        pkt[22] = (TCP_MSS >> 8) & 0xFF;
        pkt[23] = TCP_MSS & 0xFF;
    }

    if (payload_len > 0 && payload) {
        memcpy(pkt + hdr_len, payload, payload_len);
    }

    uint16_t csum = tcp_checksum(src_ip, s->remote_ip, pkt, total);
    h->checksum = htons(csum);

    int r = net_ip_send(s->remote_ip, IP_PROTO_TCP, pkt, total);
    kfree(pkt);
    return r;
}

/* ---------- 收发入口 ---------- */

void tcp_input(uint32_t src_ip, uint32_t dst_ip,
               const uint8_t *pkt, uint32_t len) {
    if (len < 20) return;

    const struct tcp_hdr *h = (const struct tcp_hdr*)pkt;

    uint16_t csum = tcp_checksum(src_ip, dst_ip, pkt, len);
    if (csum != 0) {
        KLOG_DBG("TCP: bad checksum\n");
        return;
    }

    uint8_t hdr_len = (h->data_off >> 4) * 4;
    if (hdr_len < 20 || hdr_len > len) return;

    uint16_t src_port = h->src_port;
    uint16_t dst_port = h->dst_port;
    uint32_t seq = ntohl(h->seq);
    uint32_t ack = ntohl(h->ack);
    uint8_t flags = h->flags;
    uint32_t payload_len = len - hdr_len;
    const uint8_t *payload = pkt + hdr_len;

    kprintf("[TCP] %u -> %u flags=0x%x seq=%u ack=%u len=%u\n",
            ntohs(src_port), ntohs(dst_port),
            flags, seq, ack, payload_len);

    struct tcp_socket *s = tcp_find_conn(src_ip, src_port, dst_port);

    if (!s) {
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) {
            struct tcp_socket *listen = tcp_find_by_local_port(dst_port);
            if (!listen || listen->state != TCP_LISTEN) {
                kprintf("[TCP] no listener on port %u\n", ntohs(dst_port));
                return;
            }
            s = tcp_alloc();
            if (!s) return;
            s->local_port  = dst_port;
            s->remote_ip   = src_ip;
            s->remote_port = src_port;
            s->iss         = 0x10000000;
            s->snd_una     = s->iss;
            s->snd_nxt     = s->iss + 1;
            s->rcv_nxt     = seq + 1;
            s->state       = TCP_SYN_RCVD;

            tcp_send_segment(s, dst_ip, TCP_SYN | TCP_ACK,
                             s->iss, s->rcv_nxt, NULL, 0);
            kprintf("[TCP] SYN-ACK sent, state=SYN_RCVD\n");
        }
        return;
    }

    /* RST 一律终止 */
    if (flags & TCP_RST) {
        s->state = TCP_CLOSED;
        kprintf("[TCP] RST, state=CLOSED\n");
        tcp_maybe_free(s);
        return;
    }

    /* SYN_SENT：等 SYN-ACK */
    if (s->state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack != s->snd_nxt) return;
            s->snd_una = ack;
            s->rcv_nxt = seq + 1;
            s->state = TCP_ESTABLISHED;
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
            kprintf("[TCP] connect: ESTABLISHED\n");
        }
        return;
    }

    /* SYN_RCVD：等 ACK */
    if (s->state == TCP_SYN_RCVD) {
        if (flags & TCP_ACK) {
            if (ack != s->snd_nxt) return;
            s->snd_una = ack;
            s->state = TCP_ESTABLISHED;

            struct tcp_socket *listen = tcp_find_by_local_port(s->local_port);
            if (listen && listen->accept_count < TCP_BACKLOG) {
                listen->accept_queue[listen->accept_count++] = s;
            }
            kprintf("[TCP] ESTABLISHED\n");
        }
        return;
    }

    /* ESTABLISHED：数据 / FIN */
    if (s->state == TCP_ESTABLISHED) {
        if (payload_len > 0) {
            if (seq == s->rcv_nxt) {
                uint32_t space = TCP_RX_BUF_SIZE - s->rx_count;
                uint32_t n = payload_len < space ? payload_len : space;
                for (uint32_t i = 0; i < n; i++) {
                    s->rx_buf[s->rx_tail] = payload[i];
                    s->rx_tail = (s->rx_tail + 1) % TCP_RX_BUF_SIZE;
                }
                s->rx_count += n;
                s->rcv_nxt += payload_len;
            }
        }
        if (flags & TCP_FIN) {
            /* 对端发起关闭 */
            s->rcv_nxt += 1;
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
            s->state = TCP_CLOSE_WAIT;
            kprintf("[TCP] FIN received, state=CLOSE_WAIT\n");
            return;
        }
        if (payload_len > 0) {
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
        } else if (flags & TCP_ACK) {
            s->snd_una = ack;
        }
        return;
    }

    /* FIN_WAIT_1：等对端 ACK（→ FIN_WAIT_2）或 FIN+ACK（→ CLOSED） */
    if (s->state == TCP_FIN_WAIT_1) {
        if (flags & TCP_FIN) {
            s->rcv_nxt += 1;
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
            s->state = TCP_CLOSED;
            kprintf("[TCP] FIN+ACK, state=CLOSED\n");
            tcp_maybe_free(s);
            return;
        }
        if (flags & TCP_ACK) {
            s->snd_una = ack;
            s->state = TCP_FIN_WAIT_2;
            kprintf("[TCP] ACK, state=FIN_WAIT_2\n");
        }
        return;
    }

    /* FIN_WAIT_2：等对端 FIN */
    if (s->state == TCP_FIN_WAIT_2) {
        if (flags & TCP_FIN) {
            s->rcv_nxt += 1;
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
            s->state = TCP_CLOSED;
            kprintf("[TCP] FIN, state=CLOSED\n");
            tcp_maybe_free(s);
        }
        return;
    }

    /* CLOSE_WAIT：应用调用 close 后 → LAST_ACK */
    if (s->state == TCP_CLOSE_WAIT) {
        /* 只处理重复 FIN——回 ACK */
        if (flags & TCP_FIN) {
            tcp_send_segment(s, dst_ip, TCP_ACK,
                             s->snd_nxt, s->rcv_nxt, NULL, 0);
        }
        return;
    }

    /* LAST_ACK：等最后 ACK */
    if (s->state == TCP_LAST_ACK) {
        if (flags & TCP_ACK) {
            s->snd_una = ack;
            s->state = TCP_CLOSED;
            kprintf("[TCP] ACK, state=CLOSED\n");
            tcp_maybe_free(s);
        }
        return;
    }
}

/* ---------- API ---------- */

int tcp_listen(struct tcp_socket *s, uint16_t local_port_be) {
    if (!s || local_port_be == 0) return -1;

    /* 端口冲突检查 */
    struct tcp_socket *other = tcp_find_by_local_port(local_port_be);
    if (other && other != s) return -1;

    s->local_port = local_port_be;
    s->state = TCP_LISTEN;
    return 0;
}

int tcp_connect(struct tcp_socket *s, uint32_t remote_ip,
                uint16_t remote_port_be) {
    if (!s) return -1;
    if (s->state != TCP_CLOSED) return -1;
    if (s->local_port == 0) return -1;

    s->remote_ip   = remote_ip;
    s->remote_port = remote_port_be;
    s->iss         = 0x20000000;
    s->snd_una     = s->iss;
    s->snd_nxt     = s->iss + 1;
    s->rcv_nxt     = 0;
    s->state       = TCP_SYN_SENT;

    uint32_t src_ip = net_local_ip();
    int r = tcp_send_segment(s, src_ip, TCP_SYN,
                             s->iss, 0, NULL, 0);
    if (r < 0) {
        s->state = TCP_CLOSED;
        return -1;
    }
    kprintf("[TCP] SYN sent to %08x:%u\n", remote_ip, ntohs(remote_port_be));
    return 0;
}

struct tcp_socket *tcp_accept(struct tcp_socket *listen) {
    if (!listen || listen->state != TCP_LISTEN) return NULL;
    if (listen->accept_count == 0) return NULL;

    struct tcp_socket *conn = listen->accept_queue[0];
    for (int i = 1; i < listen->accept_count; i++) {
        listen->accept_queue[i-1] = listen->accept_queue[i];
    }
    listen->accept_count--;
    return conn;
}

int tcp_send(struct tcp_socket *s, const uint8_t *data, uint32_t len) {
    if (!s || s->state != TCP_ESTABLISHED) return -1;
    if (len == 0) return 0;

    uint32_t src_ip = net_local_ip();
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > TCP_MSS) chunk = TCP_MSS;

        int r = tcp_send_segment(s, src_ip,
                                 TCP_PSH | TCP_ACK,
                                 s->snd_nxt, s->rcv_nxt,
                                 data + sent, chunk);
        if (r < 0) return sent > 0 ? (int)sent : -1;
        s->snd_nxt += chunk;
        sent += chunk;
    }
    return (int)len;
}

int tcp_recv(struct tcp_socket *s, uint8_t *buf, uint32_t max) {
    if (!s || s->state != TCP_ESTABLISHED) return -1;
    if (!buf || max == 0) return 0;
    if (s->rx_count == 0) return 0;

    uint32_t n = max < s->rx_count ? max : s->rx_count;
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = s->rx_buf[s->rx_head];
        s->rx_head = (s->rx_head + 1) % TCP_RX_BUF_SIZE;
    }
    s->rx_count -= n;
    return (int)n;
}

int tcp_close(struct tcp_socket *s) {
    if (!s) return -1;
    if (s->user_closed) return 0;    /* 已 close 过，幂等 */

    uint32_t src_ip = net_local_ip();

    if (s->state == TCP_ESTABLISHED) {
        s->user_closed = 1;
        tcp_send_segment(s, src_ip, TCP_FIN | TCP_ACK,
                         s->snd_nxt, s->rcv_nxt, NULL, 0);
        s->snd_nxt += 1;
        s->state = TCP_FIN_WAIT_1;
        kprintf("[TCP] close: FIN sent, state=FIN_WAIT_1\n");
        return 0;
    }

    if (s->state == TCP_CLOSE_WAIT) {
        s->user_closed = 1;
        tcp_send_segment(s, src_ip, TCP_FIN | TCP_ACK,
                         s->snd_nxt, s->rcv_nxt, NULL, 0);
        s->snd_nxt += 1;
        s->state = TCP_LAST_ACK;
        kprintf("[TCP] close: FIN sent, state=LAST_ACK\n");
        return 0;
    }

    if (s->state == TCP_LISTEN || s->state == TCP_CLOSED) {
        s->state = TCP_CLOSED;
        s->user_closed = 1;
        tcp_maybe_free(s);
        return 0;
    }

    /* 其他中间状态：标记 closed，等状态机自己走完 */
    s->user_closed = 1;
    return 0;
}