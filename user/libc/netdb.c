#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"

#define DNS_PORT       53
#define DNS_BUF_SIZE   512
#define DNS_TIMEOUT_MS 2000

static struct in_addr g_addr;
static char *g_aliases[] = { NULL };
static char *g_addr_list[2] = { NULL, NULL };

static struct hostent g_ent = {
    .h_name     = NULL,
    .h_aliases  = g_aliases,
    .h_addrtype = AF_INET,
    .h_length   = 4,
    .h_addr_list = g_addr_list,
};

/* 把 "example.com" 编码成 DNS QNAME（标签序列）。
 * 返回写入的字节数；失败返回 -1。 */
static int encode_qname(const char *name, uint8_t *out, int max) {
    int written = 0;
    const char *p = name;

    while (*p) {
        const char *dot = p;
        while (*dot && *dot != '.') dot++;
        int len = dot - p;
        if (len == 0) return -1;   /* 空标签 */
        if (len > 63) return -1;
        if (written + 1 + len + 1 > max) return -1;

        out[written++] = (uint8_t)len;
        memcpy(out + written, p, len);
        written += len;

        if (*dot == '.') p = dot + 1;
        else p = dot;
    }
    if (written + 1 > max) return -1;
    out[written++] = 0;   /* 根标签 */
    return written;
}

/* 跳过 DNS 报文里的 name 字段（支持压缩指针）。 */
static int skip_name(const uint8_t *pkt, int pkt_len, int off) {
    while (off < pkt_len) {
        uint8_t len = pkt[off];
        if (len == 0) return off + 1;         /* 结束 */
        if ((len & 0xC0) == 0xC0) {
            return off + 2;                    /* 压缩指针：2 字节 */
        }
        off += 1 + len;
    }
    return -1;
}

struct hostent *gethostbyname(const char *name) {
    if (!name || !*name) return NULL;

    /* 已经是点分十进制？直接解析 */
    uint32_t direct = inet_addr(name);
    if (direct != 0) {
        g_addr.s_addr = direct;
        g_addr_list[0] = (char*)&g_addr;
        g_addr_list[1] = NULL;
        g_ent.h_name = (char*)name;
        return &g_ent;
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return NULL;

    /* 绑定一个临时端口 */
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(0);   /* 内核此时不允许 0，传固定值 */
    local.sin_port = htons(43210);
    bind(s, (struct sockaddr*)&local, sizeof(local));

    /* 构造 DNS query */
    uint8_t query[DNS_BUF_SIZE];
    memset(query, 0, sizeof(query));

    query[0] = 0xAB; query[1] = 0xCD;   /* ID */
    query[2] = 0x01; query[3] = 0x00;   /* flags: standard query, RD */
    query[4] = 0x00; query[5] = 0x01;   /* QDCOUNT = 1 */
    /* ANCOUNT / NSCOUNT / ARCOUNT = 0 */

    int qlen = 12;
    int enc = encode_qname(name, query + qlen, sizeof(query) - qlen);
    if (enc < 0) { sockclose(s); return NULL; }
    qlen += enc;

    /* QTYPE = A (1), QCLASS = IN (1) */
    query[qlen++] = 0x00; query[qlen++] = 0x01;
    query[qlen++] = 0x00; query[qlen++] = 0x01;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DNS_PORT);
    dst.sin_addr = DNS_SERVER_IP;

    /* 发送（ARP 可能 miss，重试几次） */
        int sent = 0;
    for (int t = 0; t < 20; t++) {
        int r = sendto(s, query, qlen, (struct sockaddr*)&dst, sizeof(dst));
        if (r > 0) { sent = 1; break; }
        sleep_ms(50);
    }
    if (!sent) {
        dprintf(2, "[DNS] sendto failed\n");
        sockclose(s);
        return NULL;
    }

    uint8_t resp[DNS_BUF_SIZE];
    struct sockaddr_in src;
    unsigned int srclen;
    int rlen = -1;

    for (int t = 0; t < 40; t++) {
        srclen = sizeof(src);
        int r = recvfrom(s, resp, sizeof(resp),
                         (struct sockaddr*)&src, &srclen);
        if (r > 0) { rlen = r; break; }
        sleep_ms(50);
    }
    sockclose(s);
    if (rlen < 12) return NULL;

    /* 校验 ID 和 flags */
    if (resp[0] != 0xAB || resp[1] != 0xCD) return NULL;
    if ((resp[3] & 0x0F) != 0) return NULL;   /* RCODE != 0 */

    /* 跳过 header */
    int off = 12;
    /* 跳过 question */
    off = skip_name(resp, rlen, off);
    if (off < 0 || off + 4 > rlen) return NULL;
    off += 4;

    /* 遍历 answer */
    int ancount = (resp[6] << 8) | resp[7];
    for (int i = 0; i < ancount; i++) {
        off = skip_name(resp, rlen, off);
        if (off < 0 || off + 10 > rlen) return NULL;

        uint16_t type  = (resp[off] << 8) | resp[off + 1];
        uint16_t klass = (resp[off + 2] << 8) | resp[off + 3];
        /* 跳过 TTL (4) */
        uint16_t rdlen = (resp[off + 8] << 8) | resp[off + 9];
        off += 10;

        if (off + rdlen > rlen) return NULL;

        if (type == 1 && klass == 1 && rdlen == 4) {
            memcpy(&g_addr.s_addr, resp + off, 4);
            g_addr_list[0] = (char*)&g_addr;
            g_addr_list[1] = NULL;
            g_ent.h_name = (char*)name;
            return &g_ent;
        }
        off += rdlen;
    }
    return NULL;
}