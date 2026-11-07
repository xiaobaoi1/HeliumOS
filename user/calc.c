#include "syscall.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include <stdint.h>
#include "wm_proto.h"
#include "wm_font.h"
#include "unicode_font.h"
#include "shm_layout.h"
#include <math.h>

#define CW 400
#define CH 416
#define ROWS 6
#define COLS 5
#define BTN_H 48
#define BTN_GAP 4
#define TOP 92

static const char *btns[ROWS][COLS] = {
    { "C", "(", ")", "^", "<" },
    { "sin", "cos", "tan", "ln", "exp" },
    { "sqrt", "7", "8", "9", "/" },
    { "pi", "4", "5", "6", "*" },
    { "e", "1", "2", "3", "-" },
    { "0", ".", "=", "+", "" }
};

static unsigned int *g_pixels;
static struct shm_hdr *g_hdr;
static char g_shm[20], g_ev[24];
static int g_evfd = -1, g_inbox = -1;
static char g_expr[128] = "";
static char g_display[128] = "0";

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

static int text_width(const char *s) {
    int w = 0;
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp; int n = ufont_utf8_decode(p, &cp);
        int gw = wm_font_width((int)cp); if (!gw) gw = 8;
        w += gw; p += n;
    }
    return w;
}

static void draw_text(int x, int y, const char *s, unsigned int fg, unsigned int bg) {
    const unsigned char *p = (const unsigned char*)s;
    while (*p) {
        uint32_t cp; int n = ufont_utf8_decode(p, &cp);
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = 8;
        fill_rect(x, y, gw, 16, bg);
        for (int r = 0; r < 16; r++) for (int j = 0; j < gw; j++) {
            int bit = gw == 16 ? (g[r*2 + (j>>3)] & (0x80 >> (j&7)))
                               : (g[r] & (0x80 >> j));
            if (bit && x+j >= 0 && x+j < CW && y+r >= 0 && y+r < CH)
                g_pixels[(y+r)*CW + x+j] = fg;
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

static void render(void) {
    fill_rect(0, 0, CW, CH, 0x1A1A2E);
    fill_rect(8, 8, CW - 16, 76, 0x0C0C14);
    const char *show = g_display;
    int maxw = CW - 32;
    while (text_width(show) > maxw && *show) show++;
    draw_text(16, 44, show, 0xFFFFFF, 0x0C0C14);

    int bw = (CW - 20 - (COLS - 1) * BTN_GAP) / COLS;
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            int x = 10 + c * (bw + BTN_GAP);
            int y = TOP + r * (BTN_H + BTN_GAP);
            fill_rect(x, y, bw, BTN_H, 0x2A2A3E);
            fill_rect(x, y, bw, 1, 0x555A70);
            const char *t = btns[r][c];
            if (!t[0]) continue;
            draw_text(x + (bw - text_width(t)) / 2, y + 16, t, 0xFFFFFF, 0x2A2A3E);
        }
    }
    publish_all();
}

static void append(const char *s) {
    int n = strlen(g_expr), m = strlen(s);
    if (n + m >= (int)sizeof(g_expr)) return;
    memcpy(g_expr + n, s, m + 1);
    snprintf(g_display, sizeof(g_display), "%s", g_expr);
}

static void backspace(void) {
    int n = strlen(g_expr);
    if (n) g_expr[--n] = '\0';
    snprintf(g_display, sizeof(g_display), "%s", n ? g_expr : "0");
}

static void format_float(float v, char *out, int size) {
    if (math_abs(v) < 0.0000005f) v = 0.0f;
    int neg = v < 0.0f;
    if (neg) v = -v;
    if (v > 2000000000.0f) v = 2000000000.0f;
    long ip = (long)v;
    float frac = v - (float)ip;
    char digits[8];
    for (int i = 0; i < 6; i++) {
        frac *= 10.0f;
        int d = (int)frac;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        digits[i] = (char)('0' + d);
        frac -= (float)d;
    }
    int n = 6;
    while (n > 0 && digits[n - 1] == '0') n--;
    int p = 0;
    if (neg && p < size - 1) out[p++] = '-';
    char ib[24]; int il = 0;
    if (ip == 0) ib[il++] = '0';
    while (ip > 0) { ib[il++] = (char)('0' + ip % 10); ip /= 10; }
    while (il > 0 && p < size - 1) out[p++] = ib[--il];
    if (n > 0 && p < size - 2) {
        out[p++] = '.';
        for (int i = 0; i < n && p < size - 1; i++) out[p++] = digits[i];
    }
    out[p] = '\0';
}

struct parser { const char *p; int err; };
static void ps(struct parser *p) { while (*p->p == ' ' || *p->p == '\t') p->p++; }
static float pexpr(struct parser *p);
static float pnumber(struct parser *p) {
    ps(p);
    int any = 0;
    float v = 0.0f;
    while (*p->p >= '0' && *p->p <= '9') { v = v * 10.0f + (*p->p - '0'); p->p++; any = 1; }
    if (*p->p == '.') {
        p->p++; float f = 0.1f;
        while (*p->p >= '0' && *p->p <= '9') { v += (*p->p - '0') * f; f *= 0.1f; p->p++; any = 1; }
    }
    if (!any) { p->err = 1; return 0.0f; }
    return v;
}
static float pprimary(struct parser *p) {
    ps(p);
    if (*p->p == '(') {
        p->p++; float v = pexpr(p); ps(p);
        if (*p->p == ')') p->p++; else p->err = 1;
        return v;
    }
    if ((*p->p >= '0' && *p->p <= '9') || *p->p == '.') return pnumber(p);
    if ((*p->p >= 'a' && *p->p <= 'z')) {
        char name[8]; int n = 0;
        while ((*p->p >= 'a' && *p->p <= 'z') && n < 7) name[n++] = *p->p++;
        name[n] = '\0';
        if (strcmp(name, "pi") == 0) return 3.14159265358979323846f;
        if (strcmp(name, "e") == 0) return 2.71828182845904523536f;
        ps(p);
        if (*p->p != '(') { p->err = 1; return 0.0f; }
        p->p++; float v = pexpr(p); ps(p);
        if (*p->p == ')') p->p++; else p->err = 1;
        if (strcmp(name, "sin") == 0) return math_sin(v);
        if (strcmp(name, "cos") == 0) return math_cos(v);
        if (strcmp(name, "tan") == 0) return math_tan(v);
        if (strcmp(name, "sqrt") == 0) return math_sqrt(v);
        if (strcmp(name, "ln") == 0) return math_ln(v);
        if (strcmp(name, "log") == 0) return math_log10(v);
        if (strcmp(name, "exp") == 0) return math_exp(v);
        p->err = 1;
        return 0.0f;
    }
    p->err = 1;
    return 0.0f;
}
static float punary(struct parser *p) {
    ps(p);
    if (*p->p == '+') { p->p++; return punary(p); }
    if (*p->p == '-') { p->p++; return -punary(p); }
    return pprimary(p);
}
static float ppower(struct parser *p) {
    float v = punary(p); ps(p);
    if (*p->p == '^') { p->p++; float e = ppower(p); return math_pow(v, e); }
    return v;
}
static float pterm(struct parser *p) {
    float v = ppower(p);
    for (;;) {
        ps(p);
        if (*p->p == '*') { p->p++; v = math_mul(v, ppower(p)); }
        else if (*p->p == '/') { p->p++; float d = ppower(p); v = math_div(v, d); }
        else break;
    }
    return v;
}
static float pexpr(struct parser *p) {
    float v = pterm(p);
    for (;;) {
        ps(p);
        if (*p->p == '+') { p->p++; v = math_add(v, pterm(p)); }
        else if (*p->p == '-') { p->p++; v = math_sub(v, pterm(p)); }
        else break;
    }
    return v;
}

static void evaluate(void) {
    struct parser p = { g_expr, 0 };
    ps(&p);
    if (!*p.p) { strcpy(g_display, "0"); return; }
    float v = pexpr(&p);
    ps(&p);
    if (p.err || *p.p) { strcpy(g_display, "Error"); return; }
    format_float(v, g_display, sizeof(g_display));
    snprintf(g_expr, sizeof(g_expr), "%s", g_display);
}

static void press(const char *t) {
    if (!t || !t[0]) return;
    if (strcmp(t, "C") == 0) { g_expr[0] = '\0'; strcpy(g_display, "0"); }
    else if (strcmp(t, "<") == 0) backspace();
    else if (strcmp(t, "=") == 0) evaluate();
    else if (strcmp(t, "sin") == 0 || strcmp(t, "cos") == 0 ||
             strcmp(t, "tan") == 0 || strcmp(t, "sqrt") == 0 ||
             strcmp(t, "ln") == 0 || strcmp(t, "exp") == 0) {
        append(t); append("(");
    } else append(t);
    render();
}

static int create_window(void) {
    int size = shm_total_size(CW, CH);
    int shm_id = shm_open(g_shm, size, 0);
    if (shm_id < 0) return -1;
    unsigned int addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_id, 0);
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
    req.x = 280; req.y = 130; req.w = CW; req.h = CH;
    req.owner_pid = getpid();
    strncpy(req.shm_name, g_shm, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name, g_ev, sizeof(req.ev_name) - 1);
    strncpy(req.title, "Calculator", sizeof(req.title) - 1);
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
    snprintf(g_shm, sizeof(g_shm), "calc-%d", pid);
    snprintf(g_ev, sizeof(g_ev), "calc-ev-%d", pid);
    if (create_window() < 0) return 1;
    render();

    for (;;) {
        struct wm_event ev;
        while (g_evfd >= 0 && pipe_avail(g_evfd) >= (int)sizeof(ev)) {
            if (pipe_read(g_evfd, &ev, sizeof(ev)) != sizeof(ev)) break;
            if (ev.type == WM_EV_CLOSE_REQUEST || ev.type == WM_EV_CLOSE) return 0;
            if (ev.type == WM_EV_MOUSE_DOWN) {
                int bw = (CW - 20 - (COLS - 1) * BTN_GAP) / COLS;
                int r = (ev.y - TOP) / (BTN_H + BTN_GAP);
                int c = (ev.x - 10) / (bw + BTN_GAP);
                if (ev.y >= TOP && r >= 0 && r < ROWS && c >= 0 && c < COLS) {
                    int y = TOP + r * (BTN_H + BTN_GAP);
                    int x = 10 + c * (bw + BTN_GAP);
                    if (ev.x >= x && ev.x < x + bw && ev.y >= y && ev.y < y + BTN_H)
                        press(btns[r][c]);
                }
            } else if (ev.type == WM_EV_KEY) {
                int k = (int)ev.code;
                char s[2] = { (char)k, 0 };
                if (k == '\r' || k == '\n') press("=");
                else if (k == '\b' || k == 127) press("<");
                else if (strchr("0123456789.+-*/^()", k)) press(s);
            }
        }
        sleep_ms(1);
    }
}
