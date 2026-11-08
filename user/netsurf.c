#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "wm_proto.h"
#include "wm_font.h"
#include "unicode_font.h"
#include "shm_layout.h"
#include "bearssl.h"
#include "inflate.h"

#define CW 800
#define CH 600
#define TOOL_H 30
#define EAGAIN_LOCAL (-10)
#define RAW_MAX    (256 * 1024)    /* 原始 HTTP 响应 */
#define DECOMP_MAX (1024 * 1024)   /* gzip 解压后 */
#define PAGE_MAX   (1024 * 1024)   /* 剥标签后文本 */

static unsigned int *g_pixels;
static struct shm_hdr *g_hdr;
static char g_shm[24], g_ev[28];
static int g_evfd = -1, g_inbox = -1;
static char g_url[192] = "http://example.com/";
static char g_page[PAGE_MAX];
static int g_page_len = 0;
static char g_status[64] = "NetSurf ready - type a URL and press Enter";
static unsigned int g_css_fg = 0xE0E0E0;
static unsigned int g_css_bg = 0x101018;
static int g_url_fresh = 1;
/* 接收原始响应用（http/https 共用，单线程安全） */
static uint8_t g_raw[RAW_MAX];
static uint8_t g_chunked_buf[RAW_MAX];
static uint8_t g_decomp_buf[DECOMP_MAX];

static unsigned int g_next_port = 0;
static unsigned int next_port(void) {
    if (g_next_port == 0) {
        struct rtc_time rt;
        memset(&rt, 0, sizeof(rt));
        rtc_get_time(&rt);
        g_next_port = 43211 +
            ((rt.second * 13 + rt.minute * 7 + rt.hour * 31 + getpid()) % 8000);
    }
    unsigned int p = g_next_port++;
    if (g_next_port >= 55000) g_next_port = 43211;
    return p;
}

static void fill_rect(int x, int y, int w, int h, unsigned int c) {
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= CH) continue;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (px >= 0 && px < CW) g_pixels[py * CW + px] = c;
        }
    }
}

static void draw_text(int x, int y, int limit, const char *s, unsigned int fg, unsigned int bg) {
    const unsigned char *p = (const unsigned char*)s;
    while (*p && x < limit) {
        uint32_t cp; int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = 8;
        if (x + gw > limit) break;
        fill_rect(x, y, gw, 16, bg);
        if (g) for (int r = 0; r < 16; r++) {
            if (y + r < 0 || y + r >= CH) continue;
            for (int j = 0; j < gw; j++) {
                int bit = gw == 16 ? (g[r*2 + (j>>3)] & (0x80 >> (j&7)))
                                   : (g[r] & (0x80 >> j));
                if (bit) g_pixels[(y+r)*CW + x+j] = fg;
            }
        }
        x += gw; p += n;
    }
}

static void publish_all(void) {
    g_hdr->pub_x0 = 0; g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = CW; g_hdr->pub_y1 = CH;
    g_hdr->counter++;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY; g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0; g_hdr->cur_y1 = 0;
}

static void scan_css(const char *html, int n);

static void add_page_char(char c) {
    if (g_page_len < PAGE_MAX - 1) g_page[g_page_len++] = c;
}

static void html_to_text(const char *html, int n) {
    g_css_fg = 0xE0E0E0; g_css_bg = 0x101018;
    scan_css(html, n);
    g_page_len = 0;
    int in_tag = 0;
    char tag[16]; int tl = 0;
    for (int i = 0; i < n && g_page_len < PAGE_MAX - 2; i++) {
        char c = html[i];
        if (!in_tag && c == '<') { in_tag = 1; tl = 0; continue; }
        if (in_tag) {
            if (c == '>') {
                tag[tl] = '\0';
                if (!strncmp(tag, "br", 2) || !strncmp(tag, "p", 1) ||
                    !strncmp(tag, "div", 3) || !strncmp(tag, "li", 2) ||
                    !strncmp(tag, "tr", 2)) add_page_char('\n');
                in_tag = 0;
            } else if (tl < 15) {
                tag[tl++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            }
            continue;
        }
        if (c == '&') {
            if (!strncmp(html + i, "&amp;", 5)) { add_page_char('&'); i += 4; continue; }
            if (!strncmp(html + i, "&lt;", 4)) { add_page_char('<'); i += 3; continue; }
            if (!strncmp(html + i, "&gt;", 4)) { add_page_char('>'); i += 3; continue; }
            if (!strncmp(html + i, "&nbsp;", 6)) { add_page_char(' '); i += 5; continue; }
        }
        if (c == '\r') continue;
        if (c == '\n' || c == '\t' || (unsigned char)c >= 0x20) add_page_char(c);
    }
    g_page[g_page_len] = '\0';
}

static int parse_url(const char *url, char *host, int *port, char *path, int *secure) {
    int off = 0;
    if (strncmp(url, "https://", 8) == 0) { off = 8; *secure = 1; }
    else if (strncmp(url, "http://", 7) == 0) { off = 7; *secure = 0; }
    else return -1;
    const char *p = url + off; int h = 0;
    while (*p && *p != ':' && *p != '/' && h < 63) host[h++] = *p++;
    if (!h) return -1;
    host[h] = '\0'; *port = *secure ? 443 : 80;
    if (*p == ':') {
        p++; int v = 0, d = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; d++; }
        if (!d) return -1; *port = v;
    }
    if (*p == '\0' || *p == '/') strcpy(path, *p ? p : "/");
    else return -1;
    return 0;
}

static uint32_t resolve_host(const char *host) {
    uint32_t ip = inet_addr(host);
    if (ip) return ip;
    struct hostent *h = gethostbyname(host);
    if (!h || !h->h_addr_list || !h->h_addr_list[0]) return 0;
    return *(uint32_t*)h->h_addr_list[0];
}

/* ---------- HTTP 响应解析辅助 ---------- */

static int startswith_ci(const char *s, const char *prefix) {
    while (*prefix) {
        char c1 = *s++, c2 = *prefix++;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
    }
    return 1;
}

static const char *find_header_end(const char *s, int len) {
    for (int i = 0; i + 3 < len; i++) {
        if (s[i] == '\r' && s[i+1] == '\n' && s[i+2] == '\r' && s[i+3] == '\n')
            return s + i + 4;
    }
    return NULL;
}

static int header_get(const char *start, const char *end,
                      const char *name, char *out, int out_size) {
    int nlen = strlen(name);
    const char *p = start;
    while (p < end) {
        if (p + nlen + 1 <= end) {
            if (startswith_ci(p, name) && p[nlen] == ':') {
                const char *v = p + nlen + 1;
                while (v < end && (*v == ' ' || *v == '\t')) v++;
                const char *ve = v;
                while (ve < end && *ve != '\r' && *ve != '\n') ve++;
                int vlen = ve - v;
                if (vlen >= out_size) vlen = out_size - 1;
                memcpy(out, v, vlen);
                out[vlen] = '\0';
                return 1;
            }
        }
        while (p < end && *p != '\n') p++;
        if (p < end) p++;
    }
    return 0;
}

static int parse_status(const char *raw, int raw_len) {
    if (raw_len < 12 || strncmp(raw, "HTTP/", 5) != 0) return 0;
    const char *p = raw + 5;
    const char *end = raw + raw_len;
    while (p < end && *p != ' ') p++;
    if (p >= end) return 0;
    p++;
    int code = 0;
    for (int i = 0; i < 3 && p + i < end; i++) {
        if (p[i] < '0' || p[i] > '9') break;
        code = code * 10 + (p[i] - '0');
    }
    return code;
}

static int chunked_decode(const uint8_t *in, int in_len,
                          uint8_t *out, int out_max) {
    int pos = 0, out_pos = 0;
    while (pos < in_len) {
        int sz = 0, digits = 0;
        while (pos < in_len) {
            char c = in[pos];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            sz = sz * 16 + d;
            pos++;
            digits++;
        }
        if (digits == 0) return -1;
        if (pos < in_len && in[pos] == '\r') pos++;
        if (pos < in_len && in[pos] == '\n') pos++;
        if (sz == 0) break;
        if (out_pos + sz > out_max) return -1;
        if (pos + sz > in_len) return -1;
        memcpy(out + out_pos, in + pos, sz);
        out_pos += sz;
        pos += sz;
        if (pos < in_len && in[pos] == '\r') pos++;
        if (pos < in_len && in[pos] == '\n') pos++;
    }
    return out_pos;
}

struct lax_x509 {
    const br_x509_class *vtable;
    br_x509_minimal_context *xc;
};

static void lax_start_chain(const br_x509_class **ctx, const char *name) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    x->xc->vtable->start_chain(&x->xc->vtable, name);
}
static void lax_start_cert(const br_x509_class **ctx, uint32_t len) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    x->xc->vtable->start_cert(&x->xc->vtable, len);
}
static void lax_append(const br_x509_class **ctx, const unsigned char *buf, size_t len) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    x->xc->vtable->append(&x->xc->vtable, buf, len);
}
static void lax_end_cert(const br_x509_class **ctx) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    x->xc->vtable->end_cert(&x->xc->vtable);
    dprintf(2, "[X509] end_cert err=%u pkey_type=%u\n",
            x->xc->err, x->xc->pkey.key_type);
}
static unsigned lax_end_chain(const br_x509_class **ctx) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    (void)x->xc->vtable->end_chain(&x->xc->vtable);
    return 0; /* browser accepts the server certificate but still verifies the handshake key */
}
static const br_x509_pkey *lax_get_pkey(const br_x509_class *const *ctx, unsigned *usages) {
    struct lax_x509 *x = (struct lax_x509*)ctx;
    return x->xc->vtable->get_pkey(&x->xc->vtable, usages);
}
static const br_x509_class lax_x509_vtable = {
    sizeof(struct lax_x509),
    lax_start_chain, lax_start_cert, lax_append, lax_end_cert,
    lax_end_chain, lax_get_pkey
};

static int tls_read(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        int n = recvfrom(fd, buf, len, NULL, NULL);
        if (n == EAGAIN_LOCAL) { sleep_ms(10); continue; }
        return n;
    }
}
static int tls_write(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int*)ctx;
    for (;;) {
        int n = sendto(fd, buf, len, NULL, 0);
        if (n == EAGAIN_LOCAL) { sleep_ms(10); continue; }
        return n;
    }
}

static void set_x509_time(br_x509_minimal_context *xc, const struct rtc_time *rt) {
    int32_t y = rt->year, m = rt->month, d = rt->day;
    if (y < 2020 || y > 2100 || m < 1 || m > 12 || d < 1 || d > 31) {
        y = 2025; m = 1; d = 1;   /* RTC 无效时兜底 */
    }
    static const int month_days[] = {
        0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
    };
    /* 从公元 0 年 1 月 1 日到 y-m-d 的天数。
     * 0000 年是闰年，+366 基准。 */
    int32_t days = (y - 1) * 365 + (y - 1) / 4 - (y - 1) / 100
                 + (y - 1) / 400 + 366;
    days += month_days[m - 1];
    if (m > 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        days += 1;
    days += d - 1;
    uint32_t seconds = rt->hour * 3600 + rt->minute * 60 + rt->second;
    br_x509_minimal_set_time(xc, (uint32_t)days, seconds);
}

/* 前向声明 */
static int fetch_http_d(const char *url, char *out, int out_size, int depth);
static int fetch_https_d(const char *url, char *out, int out_size, int depth);

/* 解析 HTTP 响应：redirect / chunked / gzip / content-length。
 * 返回 body 长度，或 -1。 */
static int process_response(const char *raw, int raw_len,
                            const char *base_url,
                            char *out, int out_size, int depth) {
    if (raw_len <= 0) return -1;

    const char *header_end = find_header_end(raw, raw_len);
    if (!header_end) {
        if (raw_len >= out_size) raw_len = out_size - 1;
        memcpy(out, raw, raw_len);
        out[raw_len] = '\0';
        return raw_len;
    }

    int code = parse_status(raw, header_end - raw);
    dprintf(2, "[HTTP] status=%d raw_len=%d\n", code, raw_len);

    /* 3xx redirect */
    if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
        if (depth >= 5) return -1;
        char location[256];
        if (!header_get(raw, header_end, "location", location, sizeof(location))) {
            dprintf(2, "[HTTP] redirect without location\n");
            return -1;
        }
        dprintf(2, "[HTTP] redirect %d -> %s (depth=%d)\n", code, location, depth);

        char new_url[512];
        if (startswith_ci(location, "http://") || startswith_ci(location, "https://")) {
            strncpy(new_url, location, sizeof(new_url) - 1);
            new_url[sizeof(new_url) - 1] = '\0';
        } else if (location[0] == '/' && location[1] == '/') {
            char host[64], path[256];
            int port, secure;
            if (parse_url(base_url, host, &port, path, &secure) < 0) return -1;
            snprintf(new_url, sizeof(new_url), "%s:%s",
                     secure ? "https:" : "http:", location);
        } else if (location[0] == '/') {
            char host[64], path[256];
            int port, secure;
            if (parse_url(base_url, host, &port, path, &secure) < 0) return -1;
            if ((secure && port == 443) || (!secure && port == 80))
                snprintf(new_url, sizeof(new_url), "%s://%s%s",
                         secure ? "https" : "http", host, location);
            else
                snprintf(new_url, sizeof(new_url), "%s://%s:%d%s",
                         secure ? "https" : "http", host, port, location);
        } else {
            return -1;
        }

        if (startswith_ci(new_url, "https://"))
            return fetch_https_d(new_url, out, out_size, depth + 1);
        return fetch_http_d(new_url, out, out_size, depth + 1);
    }

    /* body 起点 */
    const uint8_t *body = (const uint8_t*)header_end;
    int body_len = raw_len - (header_end - raw);

    /* Content-Length 精确截断 */
    char cl_buf[32];
    if (header_get(raw, header_end, "content-length", cl_buf, sizeof(cl_buf))) {
        int cl = atoi(cl_buf);
        dprintf(2, "[HTTP] cl=%d body_len=%d\n", cl, body_len);
        if (cl > 0 && cl < body_len) body_len = cl;
    }

    /* Transfer-Encoding: chunked */
    char te_buf[64];
    if (header_get(raw, header_end, "transfer-encoding", te_buf, sizeof(te_buf))) {
        if (startswith_ci(te_buf, "chunked")) {
            int n = chunked_decode(body, body_len, g_chunked_buf, sizeof(g_chunked_buf));
            dprintf(2, "[HTTP] chunked in=%d out=%d\n", body_len, n);
            if (n < 0) return -1;
            body = g_chunked_buf;
            body_len = n;
        }
    }

    /* Content-Encoding: gzip / deflate */
    char ce_buf[64];
    if (header_get(raw, header_end, "content-encoding", ce_buf, sizeof(ce_buf))) {
        if (startswith_ci(ce_buf, "gzip")) {
            int n = gzip_decompress(body, body_len, g_decomp_buf, sizeof(g_decomp_buf));
            dprintf(2, "[HTTP] gzip in=%d out=%d\n", body_len, n);
            if (n <= 0) return -1;
            body = g_decomp_buf;
            body_len = n;
        } else if (startswith_ci(ce_buf, "deflate")) {
            int n = zlib_decompress(body, body_len, g_decomp_buf, sizeof(g_decomp_buf));
            dprintf(2, "[HTTP] deflate in=%d out=%d\n", body_len, n);
            if (n <= 0) return -1;
            body = g_decomp_buf;
            body_len = n;
        }
    }

    if (body_len >= out_size) body_len = out_size - 1;
    memcpy(out, body, body_len);
    out[body_len] = '\0';
    return body_len;
}

static int fetch_https_d(const char *url, char *out, int out_size, int depth) {
    char host[64], path[256]; int port, secure = 0;
    if (parse_url(url, host, &port, path, &secure) < 0) return -1;
    uint32_t ip = resolve_host(host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in local, dst;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_port = htons(next_port());
    bind(s, (struct sockaddr*)&local, sizeof(local));
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons(port); dst.sin_addr = ip;
    if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) { sockclose(s); return -1; }

    br_ssl_client_context sc;
    br_x509_minimal_context xc;
    br_sslio_context ioc;
    struct lax_x509 x;
    unsigned char *iobuf = (unsigned char*)malloc(BR_SSL_BUFSIZE_BIDI);
    if (!iobuf) { sockclose(s); return -1; }
        br_ssl_client_init_full(&sc, &xc, NULL, 0);

    struct rtc_time rt;
    memset(&rt, 0, sizeof(rt));
    rtc_get_time(&rt);
    set_x509_time(&xc, &rt);

    x.vtable = &lax_x509_vtable; x.xc = &xc;
    br_ssl_engine_set_x509(&sc.eng, &x.vtable);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);

    unsigned char seed[32];
    memcpy(seed, &rt, sizeof(rt) < 32 ? sizeof(rt) : 32);
    memcpy(seed + 16, &s, sizeof(s));
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
    if (!br_ssl_client_reset(&sc, host, 0)) {
        dprintf(2, "[HTTPS] client_reset failed\n");
        free(iobuf); sockclose(s); return -1; 
    }
    br_sslio_init(&ioc, &sc.eng, tls_read, &s, tls_write, &s);

    char host_hdr[80];
    if (port == 443)
        snprintf(host_hdr, sizeof(host_hdr), "%s", host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%d", host, port);

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: NetSurf-Helium/1.0\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host_hdr);
    int w = br_sslio_write_all(&ioc, req, rl);
    if (w < 0) {
        dprintf(2, "[HTTPS] write_all=%d err=0x%x\n", w,
                br_ssl_engine_last_error(&sc.eng));
        free(iobuf); sockclose(s); return -1;
    }
    if (br_sslio_flush(&ioc) < 0) {
        dprintf(2, "[HTTPS] flush err=0x%x\n",
                br_ssl_engine_last_error(&sc.eng));
        free(iobuf); sockclose(s); return -1;
    }
    int total = 0;
    while (total < (int)sizeof(g_raw)) {
        int n = br_sslio_read(&ioc, g_raw + total, sizeof(g_raw) - total);
        if (n <= 0) {
            dprintf(2, "[HTTPS] read=%d err=0x%x\n", n,
                    br_ssl_engine_last_error(&sc.eng));
            break;
        }
        total += n;
    }
    free(iobuf);
    sockclose(s);

    return process_response((const char*)g_raw, total, url, out, out_size, depth);
}

static int fetch_https(const char *url, char *out, int out_size) {
    return fetch_https_d(url, out, out_size, 0);
}

static unsigned int css_color(const char *v) {
    while (*v == ' ' || *v == '\t') v++;
    if (v[0] == '#') {
        unsigned val = 0; int n = 0; v++;
        for (; n < 6 && v[n]; n++) {
            int c = v[n], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            val = (val << 4) | (unsigned)d;
        }
        if (n >= 6) return val & 0xFFFFFFu;
    }
    if (!strncmp(v, "white", 5)) return 0xFFFFFF;
    if (!strncmp(v, "black", 5)) return 0x000000;
    if (!strncmp(v, "red", 3)) return 0xFF0000;
    if (!strncmp(v, "green", 5)) return 0x00FF00;
    if (!strncmp(v, "blue", 4)) return 0x0000FF;
    if (!strncmp(v, "gray", 4) || !strncmp(v, "grey", 4)) return 0x888888;
    if (!strncmp(v, "yellow", 6)) return 0xFFFF00;
    if (!strncmp(v, "cyan", 4)) return 0x00FFFF;
    if (!strncmp(v, "magenta", 7)) return 0xFF00FF;
    return 0xE0E0E0;
}

static void css_block(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        if (i + 5 < n && !strncmp(s + i, "color", 5) &&
            (i == 0 || s[i - 1] != '-')) {
            int j = i + 5;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == ':') {
                j++;
                while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
                g_css_fg = css_color(s + j);
            }
        }
        if (i + 16 < n && !strncmp(s + i, "background-color", 16)) {
            int j = i + 16;
            while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
            if (j < n && s[j] == ':') {
                j++;
                while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
                g_css_bg = css_color(s + j);
            }
        }
    }
}

static void scan_css(const char *html, int n) {
    for (int i = 0; i + 6 < n; i++) {
        if (!strncmp(html + i, "<style", 6)) {
            int j = i + 6;
            while (j < n && html[j] != '>') j++;
            int k = j + 1;
            while (k + 8 < n && strncmp(html + k, "</style>", 8)) k++;
            if (k > j + 1) css_block(html + j + 1, k - j - 1);
            i = k;
        } else if (!strncmp(html + i, "style=\"", 7)) {
            int k = i + 7;
            while (k < n && html[k] != '"') k++;
            css_block(html + i + 7, k - i - 7);
            i = k;
        }
    }
}

static int fetch_http_d(const char *url, char *out, int out_size, int depth) {
    char host[64], path[256]; int port, secure = 0;
    if (parse_url(url, host, &port, path, &secure) < 0) return -1;
    uint32_t ip = resolve_host(host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in local, dst;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_port = htons(next_port());
    bind(s, (struct sockaddr*)&local, sizeof(local));
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons(port); dst.sin_addr = ip;
    if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) { sockclose(s); return -1; }

    char host_hdr[80];
    if (port == 80)
        snprintf(host_hdr, sizeof(host_hdr), "%s", host);
    else
        snprintf(host_hdr, sizeof(host_hdr), "%s:%d", host, port);

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: NetSurf-Helium/1.0\r\n"
        "Accept-Encoding: gzip, deflate\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host_hdr);

    int sent = 0;
    while (sent < rl) {
        int n = sendto(s, req + sent, rl - sent, (struct sockaddr*)&dst, sizeof(dst));
        if (n == EAGAIN_LOCAL) { sleep_ms(10); continue; }
        if (n <= 0) { sockclose(s); return -1; }
        sent += n;
    }

    int total = 0, idle = 0;
    while (idle < 300 && total < (int)sizeof(g_raw)) {
        int n = recvfrom(s, g_raw + total, sizeof(g_raw) - total, NULL, NULL);
        if (n > 0) { total += n; idle = 0; }
        else if (n == 0) break;
        else if (n == EAGAIN_LOCAL) { sleep_ms(10); idle++; }
        else break;
    }
    sockclose(s);

    return process_response((const char*)g_raw, total, url, out, out_size, depth);
}

static int fetch_http(const char *url, char *out, int out_size) {
    return fetch_http_d(url, out, out_size, 0);
}

static void render(void) {
    fill_rect(0, 0, CW, CH, g_css_bg);
    fill_rect(0, 0, CW, TOOL_H, 0x2A2A3E);
    fill_rect(6, 5, CW - 74, 20, 0x080810);
    draw_text(10, 7, CW - 80, g_url, 0xFFFFFF, 0x080810);
    int cx = 10;
    for (const unsigned char *u = (const unsigned char*)g_url; *u; ) {
        uint32_t cp; int n = ufont_utf8_decode(u, &cp);
        int w = wm_font_width((int)cp); if (!w) w = 8;
        cx += w; u += n;
    }
    if (cx < CW - 72) fill_rect(cx + 1, 7, 2, 16, 0xFFFFFF);
    fill_rect(CW - 62, 5, 54, 20, 0x3A6EA5);
    draw_text(CW - 52, 7, CW - 10, "Go", 0xFFFFFF, 0x3A6EA5);

    int x = 10, y = TOOL_H + 10;
    const char *p = g_page_len ? g_page : "NetSurf browser for HeliumOS\n\nType a URL in the address bar and press Enter.\nHTTP and local SYS: paths are supported.";
    while (*p && y + 16 < CH) {
        if (*p == '\n') { x = 10; y += 18; p++; continue; }
        const unsigned char *q = (const unsigned char*)p;
        uint32_t cp; int n = ufont_utf8_decode(q, &cp);
        int gw = wm_font_width((int)cp); if (!gw) gw = 8;
        if (x + gw > CW - 10) {
            x = 10;
            y += 18;
            if (y + 16 >= CH) break;
        }
        draw_text(x, y, x + gw + 1, p, g_css_fg, g_css_bg);
        x += gw; p += n;
    }
    draw_text(10, CH - 18, CW - 10, g_status, 0x8888AA, 0x101018);
    publish_all();
}

static void load_url(void) {
    g_url_fresh = 0;
    strcpy(g_status, "Loading...");
    g_page_len = 0;
    g_page[0] = '\0';
    render();                          /* 立即显示 Loading */

    char *buf = (char*)malloc(PAGE_MAX);
    if (!buf) {
        strcpy(g_status, "Out of memory");
        render();
        return;
    }

    int n = -1;
    if (!strncmp(g_url, "http://", 7)) n = fetch_http(g_url, buf, PAGE_MAX);
    else if (!strncmp(g_url, "https://", 8)) n = fetch_https(g_url, buf, PAGE_MAX);
    else {
        int fd = fs_open(g_url, FS_O_RDONLY);
        if (fd >= 0) {
            n = fs_read(fd, buf, PAGE_MAX - 1);
            fs_close(fd);
            if (n < 0) n = -1;
            else buf[n] = '\0';
        }
    }

    if (n >= 0) {
        html_to_text(buf, n);
        snprintf(g_status, sizeof(g_status), "Done: %d bytes -> %d text",
                 n, g_page_len);
    } else {
        strcpy(g_status, "Cannot load URL");
    }
    free(buf);
    render();                          /* 显示结果 */
}

static int create_window(void) {
    int size = shm_total_size(CW, CH);
    int id = shm_open(g_shm, size, 0);
    if (id < 0) return -1;
    unsigned int addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, id, 0);
    if ((int)addr <= 0) return -1;
    g_hdr = (struct shm_hdr*)addr;
    g_pixels = (unsigned int*)(addr + SHM_HDR_SIZE);
    memset(g_pixels, 0, CW * CH * 4);
    g_hdr->counter = 0;
    g_hdr->cur_x0 = SHM_DIRTY_EMPTY; g_hdr->cur_y0 = 0;
    g_hdr->cur_x1 = 0; g_hdr->cur_y1 = 0;
    g_hdr->pub_x0 = SHM_DIRTY_EMPTY; g_hdr->pub_y0 = 0;
    g_hdr->pub_x1 = 0; g_hdr->pub_y1 = 0;
    pipe_unlink(g_ev);
    int fds[2];
    if (pipe_named(g_ev, fds) < 0) return -1;
    g_evfd = fds[0]; ipc_close(fds[1]);
    struct wm_request req;
    memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC; req.type = WM_REQ_CREATE; req.kind = WM_KIND_NORMAL;
    req.x = 100; req.y = 70; req.w = CW; req.h = CH; req.owner_pid = getpid();
    strncpy(req.shm_name, g_shm, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, g_ev, sizeof(req.ev_name) - 1);
    strncpy(req.title, "NetSurf", sizeof(req.title) - 1);
    return pipe_write(g_inbox, &req, sizeof(req)) == sizeof(req) ? 0 : -1;
}

int main(void) {
    for (int i = 0; i < 2000; i++) {
        g_inbox = pipe_open(WM_INBOX_NAME, 1);
        if (g_inbox >= 0) break;
        sleep_ms(5);
    }
    if (g_inbox < 0) return 1;
    wm_font_load();
    int pid = getpid();
    snprintf(g_shm, sizeof(g_shm), "ns-%d", pid);
    snprintf(g_ev, sizeof(g_ev), "nse-%d", pid);
    if (create_window() < 0) return 1;
    render();
    for (;;) {
        struct wm_event ev;
        while (g_evfd >= 0 && pipe_avail(g_evfd) >= (int)sizeof(ev)) {
            if (pipe_read(g_evfd, &ev, sizeof(ev)) != sizeof(ev)) break;
            if (ev.type == WM_EV_CLOSE_REQUEST || ev.type == WM_EV_CLOSE) return 0;
            if (ev.type == WM_EV_MOUSE_DOWN) {
                if (ev.y < TOOL_H && ev.x >= CW - 62) load_url();
                else if (ev.y < TOOL_H) { g_url_fresh = 1; render(); }
            } else if (ev.type == WM_EV_KEY) {
                int k = (int)ev.code;
                if (k == '\r' || k == '\n') load_url();
                else if (k == '\b' || k == 127) {
                    int n = strlen(g_url);
                    if (n) g_url[n - 1] = '\0';
                    render();
                } else if (k >= 0x20 && k < 0x7F) {
                    if (g_url_fresh) { g_url[0] = '\0'; g_url_fresh = 0; }
                    int n = strlen(g_url);
                    if (n < (int)sizeof(g_url) - 1) {
                        g_url[n] = (char)k; g_url[n + 1] = '\0';
                    }
                    g_status[0] = '\0';
                    render();
                }
            }
        }
        sleep_ms(1);
    }
}
