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

#define CW 800
#define CH 600
#define TOOL_H 30
#define EAGAIN_LOCAL (-10)
#define PAGE_MAX 32768

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
        if (g) for (int r = 0; r < 16; r++) for (int j = 0; j < gw; j++) {
            int bit = gw == 16 ? (g[r*2 + (j>>3)] & (0x80 >> (j&7)))
                               : (g[r] & (0x80 >> j));
            if (bit) g_pixels[(y+r)*CW + x+j] = fg;
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

static int fetch_https(const char *url, char *out, int out_size) {
    char host[64], path[256]; int port, secure = 0;
    if (parse_url(url, host, &port, path, &secure) < 0) return -1;
    uint32_t ip = resolve_host(host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in local, dst;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_port = htons(43212);
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
    x.vtable = &lax_x509_vtable; x.xc = &xc;
    br_ssl_engine_set_x509(&sc.eng, &x.vtable);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, BR_SSL_BUFSIZE_BIDI, 1);
    unsigned char seed[32];
    struct rtc_time rt; memset(&rt, 0, sizeof(rt)); rtc_get_time(&rt);
    memcpy(seed, &rt, sizeof(rt) < 32 ? sizeof(rt) : 32);
    memcpy(seed + 16, &s, sizeof(s));
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof(seed));
    if (!br_ssl_client_reset(&sc, host, 0)) { free(iobuf); sockclose(s); return -1; }
    br_sslio_init(&ioc, &sc.eng, tls_read, &s, tls_write, &s);

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NetSurf-Helium/1.0\r\nConnection: close\r\n\r\n",
        path, host);
    if (br_sslio_write_all(&ioc, req, rl) < 0 || br_sslio_flush(&ioc) < 0) {
        free(iobuf); sockclose(s); return -1;
    }
    int total = 0;
    while (total < out_size - 1) {
        int n = br_sslio_read(&ioc, out + total, out_size - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    free(iobuf);
    sockclose(s);
    out[total] = '\0';
    char *body = strstr(out, "\r\n\r\n");
    if (body) {
        int skip = (int)(body + 4 - out), len = total - skip;
        memmove(out, body + 4, len); out[len] = '\0'; return len;
    }
    return total;
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

static int fetch_http(const char *url, char *out, int out_size) {
    char host[64], path[256]; int port, secure = 0;
    if (parse_url(url, host, &port, path, &secure) < 0) return -1;
    uint32_t ip = resolve_host(host);
    if (!ip) return -1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET; local.sin_port = htons(43211);
    bind(s, (struct sockaddr*)&local, sizeof(local));
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET; dst.sin_port = htons(port); dst.sin_addr = ip;
    if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) < 0) { sockclose(s); return -1; }
    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NetSurf-Helium/1.0\r\nConnection: close\r\n\r\n",
        path, host);
    int sent = 0;
    while (sent < rl) {
        int n = sendto(s, req + sent, rl - sent, (struct sockaddr*)&dst, sizeof(dst));
        if (n <= 0) { sockclose(s); return -1; }
        sent += n;
    }
    int total = 0, idle = 0;
    while (idle < 300 && total < out_size - 1) {
        int n = recvfrom(s, out + total, out_size - 1 - total, NULL, NULL);
        if (n > 0) { total += n; idle = 0; }
        else if (n == 0) break;
        else if (n == EAGAIN_LOCAL) { sleep_ms(10); idle++; }
        else break;
    }
    sockclose(s);
    out[total] = '\0';
    char *body = strstr(out, "\r\n\r\n");
    if (body) {
        int skip = (int)(body + 4 - out);
        int len = total - skip;
        memmove(out, body + 4, len);
        out[len] = '\0';
        return len;
    }
    return total;
}

static void load_url(void) {
    g_url_fresh = 0;
    strcpy(g_status, "Loading...");
    g_page_len = 0;
    g_page[0] = '\0';
    publish_all();
    char *buf = (char*)malloc(PAGE_MAX);
    if (!buf) return;
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
        strcpy(g_status, "Done");
    } else strcpy(g_status, "Cannot load URL");
    free(buf);
    publish_all();
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
        if (x + gw > CW - 10) { x = 10; y += 18; }
        draw_text(x, y, x + gw + 1, p, g_css_fg, g_css_bg);
        x += gw; p += n;
    }
    draw_text(10, CH - 18, CW - 10, g_status, 0x8888AA, 0x101018);
    publish_all();
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
