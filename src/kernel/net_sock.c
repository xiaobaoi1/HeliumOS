#include <net_sock.h>
#include <net_proto.h>
#include <task.h>
#include <heap.h>
#include <errno.h>
#include <printf.h>
#include <string.h>
#include <uaccess.h>

static struct udp_socket g_pool[UDP_MAX_SOCKETS];

void net_sock_init(void) {
    memset(g_pool, 0, sizeof(g_pool));
}

/* ---------- socket 对象池 ---------- */

struct udp_socket *udp_socket_alloc(void) {
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (!g_pool[i].used) {
            struct udp_socket *s = &g_pool[i];
            memset(s, 0, sizeof(*s));
            s->used = 1;
            return s;
        }
    }
    return NULL;
}

void udp_socket_free(struct udp_socket *s) {
    if (!s) return;
    for (int i = 0; i < s->rx_count; i++) {
        int idx = (s->rx_head + i) % UDP_RX_QUEUE_LEN;
        if (s->rx_queue[idx]) {
            kfree(s->rx_queue[idx]);
            s->rx_queue[idx] = NULL;
        }
    }
    s->used = 0;
}

struct udp_socket *udp_socket_by_port(uint16_t port_be) {
    if (port_be == 0) return NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (g_pool[i].used && g_pool[i].local_port == port_be) {
            return &g_pool[i];
        }
    }
    return NULL;
}

/* ---------- 每进程句柄 ---------- */

static int sock_handle_alloc(struct task *t, struct udp_socket *s) {
    for (int i = 0; i < SOCK_MAX_HANDLES; i++) {
        if (!t->sock_handles[i].used) {
            t->sock_handles[i].used = 1;
            t->sock_handles[i].type = SOCK_DGRAM;
            t->sock_handles[i].sock = s;
            s->refcount++;
            return i;
        }
    }
    return -1;
}

static struct udp_socket *sock_handle_deref(struct task *t, int h) {
    if (!t || h < 0 || h >= SOCK_MAX_HANDLES) return NULL;
    if (!t->sock_handles[h].used) return NULL;
    return t->sock_handles[h].sock;
}

void net_sock_release_all(struct task *t) {
    if (!t) return;
    for (int i = 0; i < SOCK_MAX_HANDLES; i++) {
        if (t->sock_handles[i].used) {
            struct udp_socket *s = t->sock_handles[i].sock;
            t->sock_handles[i].used = 0;
            t->sock_handles[i].sock = NULL;
            if (s) {
                if (s->refcount > 0) s->refcount--;
                if (s->refcount == 0) udp_socket_free(s);
            }
        }
    }
}

static int copy_sockaddr_in_from_user(struct sockaddr_in *out,
                                       const void *addr_user,
                                       uint32_t addrlen) {
    if (!addr_user) return EINVAL;
    if (addrlen < sizeof(struct sockaddr_in)) return EINVAL;
    if (check_user_range((uint32_t)addr_user, sizeof(*out)) < 0)
        return EFAULT;
    if (copy_from_user(out, (uint32_t)addr_user, sizeof(*out)) < 0)
        return EFAULT;
    if (out->sin_family != AF_INET) return EINVAL;
    return OK;
}

static int copy_sockaddr_in_to_user(void *addr_user, uint32_t *addrlen_user,
                                     const struct sockaddr_in *src) {
    if (!addr_user) return OK;
    if (!addrlen_user) return EINVAL;
    if (check_user_range((uint32_t)addrlen_user, 4) < 0) return EFAULT;

    uint32_t avail;
    if (copy_from_user(&avail, (uint32_t)addrlen_user, 4) < 0) return EFAULT;
    if (avail < sizeof(*src)) return EINVAL;

    if (check_user_range((uint32_t)addr_user, sizeof(*src)) < 0) return EFAULT;
    if (copy_to_user((uint32_t)addr_user, src, sizeof(*src)) < 0) return EFAULT;

    uint32_t actual = sizeof(*src);
    if (copy_to_user((uint32_t)addrlen_user, &actual, 4) < 0) return EFAULT;
    return OK;
}

/* ---------- syscall ---------- */

int sys_socket(int domain, int type, int proto) {
    (void)proto;
    if (domain != AF_INET) return EINVAL;
    if (type != SOCK_DGRAM && type != SOCK_ICMP) return EINVAL;

    struct task *cur = get_current_task();
    if (!cur) return EINVAL;

    struct udp_socket *s = udp_socket_alloc();
    if (!s) return ENOSPC;

    int h = sock_handle_alloc(cur, s);
    if (h < 0) {
        udp_socket_free(s);
        return ENOSPC;
    }
    cur->sock_handles[h].type = type;   /* 覆盖 sock_handle_alloc 里的默认 */
    return h;
}

int sys_bind(int h, const void *addr_user, uint32_t addrlen) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= SOCK_MAX_HANDLES) return EINVAL;
    if (!cur->sock_handles[h].used) return EINVAL;
    if (cur->sock_handles[h].type != SOCK_DGRAM) return EINVAL;

    struct sockaddr_in addr;
    int r = copy_sockaddr_in_from_user(&addr, addr_user, addrlen);
    if (r != OK) return r;
    if (addr.sin_port == 0) return EINVAL;

    struct udp_socket *s = cur->sock_handles[h].sock;
    if (!s) return EINVAL;

    struct udp_socket *other = udp_socket_by_port(addr.sin_port);
    if (other && other != s) return EADDRINUSE;

    s->local_port = addr.sin_port;
    return OK;
}

int sys_sendto(int h, const void *buf_user, uint32_t len,
               const void *dst_user, uint32_t addrlen) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= SOCK_MAX_HANDLES) return EINVAL;
    if (!cur->sock_handles[h].used) return EINVAL;
    if (len > UDP_PAYLOAD_MAX) return EINVAL;

    struct sockaddr_in dst;
    int r = copy_sockaddr_in_from_user(&dst, dst_user, addrlen);
    if (r != OK) return r;

    struct udp_socket *s = cur->sock_handles[h].sock;
    if (!s) return EINVAL;

    uint8_t tmp[UDP_PAYLOAD_MAX];
    if (len > 0) {
        if (!buf_user) return EINVAL;
        if (check_user_range((uint32_t)buf_user, len) < 0) return EFAULT;
        if (copy_from_user(tmp, (uint32_t)buf_user, len) < 0) return EFAULT;
    }

    if (cur->sock_handles[h].type == SOCK_ICMP) {
        int ret = net_icmp_send(dst.sin_addr, tmp, len);
        return ret < 0 ? ret : (int)len;
    }

    if (s->local_port == 0) return EINVAL;
    int ret = net_udp_send(dst.sin_addr, s->local_port, dst.sin_port, tmp, len);
    return ret < 0 ? ret : (int)len;
}

int sys_recvfrom(int h, void *buf_user, uint32_t max,
                 void *src_user, uint32_t *addrlen_user) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= SOCK_MAX_HANDLES) return EINVAL;
    if (!cur->sock_handles[h].used) return EINVAL;

    struct udp_socket *s = cur->sock_handles[h].sock;
    if (!s) return EINVAL;

    uint32_t range = max > UDP_PAYLOAD_MAX ? UDP_PAYLOAD_MAX : max;
    if (range > 0) {
        if (!buf_user) return EINVAL;
        if (check_user_range((uint32_t)buf_user, range) < 0) return EFAULT;
    }

    if (s->rx_count == 0) {
        KLOG_DBG("[RECVFROM] empty\n");
        return EAGAIN;
    }
    

    struct udp_packet *p = s->rx_queue[s->rx_head];
    s->rx_queue[s->rx_head] = NULL;
    s->rx_head = (s->rx_head + 1) % UDP_RX_QUEUE_LEN;
    s->rx_count--;

    uint32_t copy_len = p->len < range ? p->len : range;
    if (copy_len > 0 && buf_user) {
        if (copy_to_user((uint32_t)buf_user, p->data, copy_len) < 0) {
            kfree(p);
            return EFAULT;
        }
    }

    if (src_user) {
        struct sockaddr_in src;
        memset(&src, 0, sizeof(src));
        src.sin_family = AF_INET;
        src.sin_port = p->src_port;
        src.sin_addr = p->src_ip;
        int r = copy_sockaddr_in_to_user(src_user, addrlen_user, &src);
        if (r != OK) { kfree(p); return r; }
    }

    kfree(p);
    return (int)copy_len;
}

int sys_sockclose(int h) {
    struct task *cur = get_current_task();
    if (!cur) return EINVAL;
    if (h < 0 || h >= SOCK_MAX_HANDLES) return EINVAL;
    if (!cur->sock_handles[h].used) return EINVAL;

    struct udp_socket *s = cur->sock_handles[h].sock;
    cur->sock_handles[h].used = 0;
    cur->sock_handles[h].sock = NULL;

    if (s) {
        if (s->refcount > 0) s->refcount--;
        if (s->refcount == 0) udp_socket_free(s);
    }
    return OK;
}

/* ---------- UDP 收发 ---------- */

void net_udp_rx(uint32_t src_ip, uint16_t src_port,
                uint16_t dst_port, const uint8_t *payload, uint32_t len) {
    struct udp_socket *s = udp_socket_by_port(dst_port);
    if (!s) return;

    if (s->rx_count >= UDP_RX_QUEUE_LEN) {
        KLOG_DBG("UDP: queue full, drop\n");
        return;
    }

    if (len > UDP_PAYLOAD_MAX) len = UDP_PAYLOAD_MAX;

    struct udp_packet *p = (struct udp_packet*)kmalloc(sizeof(struct udp_packet));
    if (!p) return;

    p->src_ip = src_ip;
    p->src_port = src_port;
    p->len = (uint16_t)len;
    if (len > 0) memcpy(p->data, payload, len);

    s->rx_queue[s->rx_tail] = p;
    s->rx_tail = (s->rx_tail + 1) % UDP_RX_QUEUE_LEN;
    s->rx_count++;
}

int net_udp_send(uint32_t dst_ip_be, uint16_t src_port_be,
                 uint16_t dst_port_be, const uint8_t *data, uint32_t len) {
    if (len > UDP_PAYLOAD_MAX) return EINVAL;

    uint32_t total = 8 + len;
    uint8_t *pkt = (uint8_t*)kmalloc(total);
    if (!pkt) return ENOMEM;

    pkt[0] = (src_port_be >> 8) & 0xFF;
    pkt[1] = src_port_be & 0xFF;
    pkt[2] = (dst_port_be >> 8) & 0xFF;
    pkt[3] = dst_port_be & 0xFF;
    pkt[4] = (total >> 8) & 0xFF;
    pkt[5] = total & 0xFF;
    pkt[6] = 0; pkt[7] = 0;   /* 校验和 = 0：不校验 */

    if (len > 0) memcpy(pkt + 8, data, len);

    int r = net_ip_send(dst_ip_be, IP_PROTO_UDP, pkt, total);
    kfree(pkt);
    return r < 0 ? r : (int)len;
}

/* 由 net_proto.c 的 net_icmp_rx 调用。把 echo reply 塞进 ICMP socket。 */
void net_icmp_deliver(uint32_t src_ip, const uint8_t *p, uint32_t len) {   
    struct udp_socket *s = NULL;
    for (int i = 0; i < UDP_MAX_SOCKETS; i++) {
        if (g_pool[i].used && g_pool[i].local_port == 0) {
            s = &g_pool[i];
            break;
        }
    }
    if (!s) return;
    if (s->rx_count >= UDP_RX_QUEUE_LEN) return;
    if (len > UDP_PAYLOAD_MAX) len = UDP_PAYLOAD_MAX;

    struct udp_packet *pkt = (struct udp_packet*)kmalloc(sizeof(struct udp_packet));
    if (!pkt) return;

    pkt->src_ip = src_ip;
    pkt->src_port = 0;
    pkt->len = (uint16_t)len;
    if (len > 0) memcpy(pkt->data, p, len);

    s->rx_queue[s->rx_tail] = pkt;
    s->rx_tail = (s->rx_tail + 1) % UDP_RX_QUEUE_LEN;
    s->rx_count++;
}