#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include "stdio.h"
#include "string.h"
#include <stdint.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_ACK      5

struct dhcp_pkt {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t cookie;
    uint8_t  options[312];
} __attribute__((packed));

static void dhcp_build(struct dhcp_pkt *p, uint8_t msg_type,
                       const uint8_t *mac, uint32_t req_ip) {
    memset(p, 0, sizeof(*p));
    p->op = 1;
    p->htype = 1;
    p->hlen = 6;
    p->xid = 0x12345678;
    p->flags = htons(0x8000);
    memcpy(p->chaddr, mac, 6);
    p->cookie = htonl(0x63825363);

    uint8_t *o = p->options;
    *o++ = 53; *o++ = 1; *o++ = msg_type;
    if (msg_type == DHCP_REQUEST && req_ip) {
        *o++ = 50; *o++ = 4;
        memcpy(o, &req_ip, 4); o += 4;
    }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
}

static const uint8_t *dhcp_opt(const struct dhcp_pkt *p, int want) {
    const uint8_t *o = p->options;
    const uint8_t *end = o + sizeof(p->options);
    while (o < end) {
        if (*o == 255) return NULL;
        if (*o == 0) { o++; continue; }
        int type = o[0], len = o[1];
        if (type == want) return o + 2;
        o += 2 + len;
    }
    return NULL;
}

static int wait_msg(int s, uint8_t want, struct dhcp_pkt *out, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) {
        unsigned char buf[600];
        struct sockaddr_in src;
        unsigned int srclen = sizeof(src);
        int r = recvfrom(s, buf, sizeof(buf), (struct sockaddr*)&src, &srclen);
        if (r >= 240) {
            uint32_t n = (uint32_t)r;
            if (n > sizeof(*out)) n = sizeof(*out);
            memcpy(out, buf, n);
            const uint8_t *t53 = dhcp_opt(out, 53);
            if (t53 && *t53 == want) return 0;
        }
        sleep_ms(10);
    }
    return -1;
}

static int dhcp_discover(int s, const uint8_t *mac,
                         struct dhcp_pkt *offer, int attempts) {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DHCP_SERVER_PORT);
    dst.sin_addr = inet_addr("255.255.255.255");

    for (int i = 0; i < attempts; i++) {
        struct dhcp_pkt disc;
        dhcp_build(&disc, DHCP_DISCOVER, mac, 0);
        int sr = sendto(s, &disc, sizeof(disc),
                        (struct sockaddr*)&dst, sizeof(dst));
        if (sr < 0) {
            printf("dhcp: DISCOVER send failed (%d)\n", sr);
            continue;
        }
        if (wait_msg(s, DHCP_OFFER, offer, 1000) == 0) return 0;
    }
    return -1;
}

int main(void) {
    struct net_if_info info;
    if (net_get_if(&info) < 0) {
        printf("dhcp: net_get_if failed\n");
        return 1;
    }

    uint8_t mac[6];
    memcpy(mac, info.mac, 6);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) { printf("dhcp: socket %d\n", s); return 1; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(DHCP_CLIENT_PORT);
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        printf("dhcp: bind failed\n");
        sockclose(s);
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DHCP_SERVER_PORT);
    dst.sin_addr = inet_addr("255.255.255.255");

    /* DISCOVER */
    struct dhcp_pkt offer;
    if (dhcp_discover(s, mac, &offer, 5) < 0) {
        printf("dhcp: no OFFER after 5 attempts\n");
        sockclose(s);
        return 1;
    }

    /* REQUEST */
    struct dhcp_pkt req;
    dhcp_build(&req, DHCP_REQUEST, mac, offer.yiaddr);
    int sr = sendto(s, &req, sizeof(req), (struct sockaddr*)&dst, sizeof(dst));
    if (sr < 0) {
        printf("dhcp: DISCOVER send failed (%d)\n", sr);
        sockclose(s);
        return 1;
    }

    struct dhcp_pkt ack;
    if (wait_msg(s, DHCP_ACK, &ack, 3000) < 0) {
        printf("dhcp: no ACK\n");
        sockclose(s);
        return 1;
    }

    uint32_t ip = ack.yiaddr;
    const uint8_t *m = dhcp_opt(&ack, 1);
    const uint8_t *r = dhcp_opt(&ack, 3);
    const uint8_t *d = dhcp_opt(&ack, 6);
    if (!m || !r) {
        printf("dhcp: ACK missing mask/router\n");
        sockclose(s);
        return 1;
    }

    uint32_t mask, gw, dns = 0;
    memcpy(&mask, m, 4);
    memcpy(&gw, r, 4);
    if (d) memcpy(&dns, d, 4);

    net_set_if(ip, mask, gw);
    if (dns) net_set_dns(dns);

    char ip_s[16], mask_s[16], gw_s[16];
    strcpy(ip_s, inet_ntoa(ip));
    strcpy(mask_s, inet_ntoa(mask));
    strcpy(gw_s, inet_ntoa(gw));
    printf("dhcp: bound %s/%s gw %s\n", ip_s, mask_s, gw_s);

    sockclose(s);
    return 0;
}