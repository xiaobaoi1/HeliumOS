/* http — 最小 HTTP/1.0 客户端
 * 用法：HTTP <url>
 * 示例：HTTP http://example.com/
 */
#include "syscall.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define EAGAIN_LOCAL  (-10)

/* 解析 http://host[:port][/path]
 * 返回 0 成功，-1 失败。 */
static int parse_url(const char *url, char *host, int *port, char *path) {
    if (strncmp(url, "http://", 7) != 0) return -1;
    const char *p = url + 7;

    /* host */
    int hlen = 0;
    while (*p && *p != ':' && *p != '/' && hlen < 63) {
        host[hlen++] = *p++;
    }
    if (hlen == 0) return -1;
    host[hlen] = '\0';

    /* port */
    *port = 80;
    if (*p == ':') {
        p++;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            p++;
            digits++;
        }
        if (digits == 0 || v <= 0 || v > 65535) return -1;
        *port = v;
    }

    /* path */
    if (*p == '\0' || *p == '/') {
        strcpy(path, *p ? p : "/");
    } else {
        return -1;
    }

    return 0;
}

static uint32_t resolve(const char *host) {
    uint32_t ip = inet_addr(host);
    if (ip != 0) return ip;
    struct hostent *h = gethostbyname(host);
    if (!h || !h->h_addr_list || !h->h_addr_list[0]) return 0;
    return *(uint32_t*)h->h_addr_list[0];
}

static int http_get(const char *host, int port, const char *path) {
    uint32_t ip = resolve(host);
    if (ip == 0) {
        dprintf(2, "http: cannot resolve '%s'\n", host);
        return 1;
    }

    dprintf(2, "http: %s (%s) :%d %s\n",
            host, inet_ntoa(ip), port, path);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) { dprintf(2, "http: socket %d\n", s); return 1; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port = htons(43210);
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) < 0) {
        dprintf(2, "http: bind failed\n");
        sockclose(s);
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    dst.sin_addr = ip;

    int r = connect(s, (struct sockaddr*)&dst, sizeof(dst));
    if (r < 0) {
        dprintf(2, "http: connect failed (%d)\n", r);
        sockclose(s);
        return 1;
    }

    char req[512];
    int req_len = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "User-Agent: helium-http/0.1\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host);

    int sent = 0;
    while (sent < req_len) {
        int n = sendto(s, req + sent, req_len - sent,
                       (struct sockaddr*)&dst, sizeof(dst));
        if (n <= 0) {
            dprintf(2, "http: send failed (%d)\n", n);
            sockclose(s);
            return 1;
        }
        sent += n;
    }

    /* 接收响应，写到 stdout */
    int total = 0;
    int idle_ticks = 0;
    char buf[1024];

    while (idle_ticks < 300) {   /* 3 秒无数据 → 认为结束 */
        int n = recvfrom(s, buf, sizeof(buf), NULL, NULL);
        if (n > 0) {
            write(1, buf, n);
            total += n;
            idle_ticks = 0;
        } else if (n == 0) {
            break;               /* EOF */
        } else if (n == EAGAIN_LOCAL) {
            sleep_ms(10);
            idle_ticks++;
        } else {
            break;               /* 其他错误 */
        }
    }

    dprintf(2, "\nhttp: received %d bytes\n", total);
    sockclose(s);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(2, "usage: http <url>\n");
        dprintf(2, "  example: http http://example.com/\n");
        return 1;
    }

    char host[64], path[256];
    int port;
    if (parse_url(argv[1], host, &port, path) < 0) {
        dprintf(2, "http: bad url '%s'\n", argv[1]);
        dprintf(2, "  must start with 'http://'\n");
        return 1;
    }

    return http_get(host, port, path);
}