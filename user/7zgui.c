/* 7zgui.c - 7-Zip GUI（文件管理器风格，带 Deflate 压缩与下拉菜单） */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>
#include <stdarg.h>
#include "shm_layout.h"
#include "wm_proto.h"
#include "wm_font.h"

#include "7zip/C/7z.h"
#include "7zip/C/7zAlloc.h"
#include "7zip/C/7zBuf.h"
#include "7zip/C/7zCrc.h"
#include "7zip/C/7zFile.h"
#include "7zip/C/7zTypes.h"

/* ==================== 布局常量 ==================== */
#define WIN_W        780
#define WIN_H        540
#define TOOLBAR_H    36
#define PATH_H       22
#define ROW_H        20
#define STATUS_H     28
#define MAX_ENTRIES  512

#define COL_BG        0x1E1E28
#define COL_TOOLBAR   0x2A2A3A
#define COL_BTN       0x3A4A6A
#define COL_BTN_DIS   0x2A2A3A
#define COL_BTN_FG    0xE0E0F0
#define COL_BTN_FG_D  0x606070
#define COL_MENU_BG   0x303044
#define COL_MENU_HL   0x4A6AC8
#define COL_PATH      0x202030
#define COL_PATH_FG   0xC8C8DC
#define COL_ROW_EVEN  0x222232
#define COL_ROW_ODD   0x26263A
#define COL_ROW_SEL   0x4A6AC8
#define COL_ROW_FG    0xF0F0F8
#define COL_ROW_DIM   0xA0A0B8
#define COL_STATUS    0x2A2A3A
#define COL_STATUS_FG 0xC0C0D8
#define COL_LINE      0x505060

#define Z_BUF_SIZE    32768
#define Z_HASH_SIZE   65536

/* ==================== 数据结构 ==================== */
struct file_entry {
    char     name[256];
    int      is_dir;
    uint32_t size;
};

struct app {
    int win_w, win_h;

    int  shm_id;
    struct shm_hdr *shm;
    unsigned int   *pixels;

    int ev_fd;
    int wm_fd;

    char cwd[256];
    struct file_entry entries[MAX_ENTRIES];
    int n_entries;
    int sel;
    int scroll;

    int  in_archive;
    char archive_path[256];

    char status[128];

    int tick;
    int last_click_idx;
    int last_click_tick;
    
    int menu_open;
};

static const ISzAlloc g_alloc = { SzAlloc, SzFree };

/* ==================== 前置声明 ==================== */
static void read_dir(struct app *a);
static void redraw_all(struct app *a);
static void do_extract(struct app *a);
static void on_double_click(struct app *a, int idx);
static void do_open_archive(struct app *a, const char *path);

/* ==================== 基础绘图 ==================== */
static void fill_rect(unsigned int *buf, int bw, int bh,
                      int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > bw) x1 = bw;
    if (y1 > bh) y1 = bh;
    if (x1 <= x0 || y1 <= y0) return;
    for (int j = y0; j < y1; j++) {
        unsigned int *row = buf + j * bw;
        for (int i = x0; i < x1; i++) row[i] = rgb;
    }
}

static void put_pixel(unsigned int *buf, int bw, int bh,
                      int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || x >= bw || y >= bh) return;
    buf[y * bw + x] = rgb;
}

/* ==================== 字体 ==================== */
static int utf8_decode(const unsigned char *s, uint32_t *out) {
    if (!s[0]) { *out = 0; return 0; }
    if (s[0] < 0x80) { *out = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && s[1]) {
        *out = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F); return 2;
    }
    if ((s[0] & 0xF0) == 0xE0 && s[1] && s[2]) {
        *out = ((uint32_t)(s[0] & 0x0F) << 12) |
               ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *out = ((uint32_t)(s[0] & 0x07) << 18) |
               ((uint32_t)(s[1] & 0x3F) << 12) |
               ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4;
    }
    *out = '?'; return 1;
}

static void draw_text(unsigned int *buf, int bw, int bh,
                      int x, int y, int limit,
                      const char *s, uint32_t color) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int n = utf8_decode(p, &cp);
        if (n == 0) break;
        const uint8_t *g = wm_font_glyph((int)cp);
        int gw = wm_font_width((int)cp);
        if (!g) { g = wm_font_glyph('?'); gw = wm_font_width('?'); }
        if (!gw) gw = 8;
        if (x + gw > limit) break;
        if (g) {
            for (int r = 0; r < 16; r++) {
                for (int j = 0; j < gw; j++) {
                    int bit = (gw == 16)
                        ? (g[r * 2 + (j >> 3)] & (0x80 >> (j & 7)))
                        : (g[r] & (0x80 >> j));
                    if (bit) put_pixel(buf, bw, bh, x + j, y + r, color);
                }
            }
        }
        x += gw;
        p += n;
    }
}

/* ==================== 脏区发布 ==================== */
static void mark_dirty(struct app *a, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > a->win_w) x1 = a->win_w;
    if (y1 > a->win_h) y1 = a->win_h;
    if (x1 <= x0 || y1 <= y0) return;

    if (a->shm->cur_x0 == SHM_DIRTY_EMPTY) {
        a->shm->cur_x0 = (uint32_t)x0;
        a->shm->cur_y0 = (uint32_t)y0;
        a->shm->cur_x1 = (uint32_t)x1;
        a->shm->cur_y1 = (uint32_t)y1;
    } else {
        if ((uint32_t)x0 < a->shm->cur_x0) a->shm->cur_x0 = (uint32_t)x0;
        if ((uint32_t)y0 < a->shm->cur_y0) a->shm->cur_y0 = (uint32_t)y0;
        if ((uint32_t)x1 > a->shm->cur_x1) a->shm->cur_x1 = (uint32_t)x1;
        if ((uint32_t)y1 > a->shm->cur_y1) a->shm->cur_y1 = (uint32_t)y1;
    }
}

static void publish(struct app *a) {
    if (a->shm->cur_x0 == SHM_DIRTY_EMPTY) return;
    if (a->shm->pub_x0 != SHM_DIRTY_EMPTY) return;
    a->shm->pub_x0 = a->shm->cur_x0;
    a->shm->pub_y0 = a->shm->cur_y0;
    a->shm->pub_x1 = a->shm->cur_x1;
    a->shm->pub_y1 = a->shm->cur_y1;
    a->shm->cur_x0 = SHM_DIRTY_EMPTY;
    a->shm->cur_y0 = 0;
    a->shm->cur_x1 = 0;
    a->shm->cur_y1 = 0;
    a->shm->counter++;
}

/* ==================== 状态栏 ==================== */
static void draw_status_bar(struct app *a) {
    unsigned int *buf = a->pixels;
    int bw = a->win_w, bh = a->win_h;
    int sy = bh - STATUS_H;
    fill_rect(buf, bw, bh, 0, sy, bw, STATUS_H, COL_STATUS);
    fill_rect(buf, bw, bh, 0, sy, bw, 1, COL_LINE);
    draw_text(buf, bw, bh, 8, sy + 6, bw - 8, a->status, COL_STATUS_FG);
}

static void set_status(struct app *a, const char *s) {
    strncpy(a->status, s, sizeof(a->status) - 1);
    a->status[sizeof(a->status) - 1] = 0;
}

static void set_progress(struct app *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof(a->status), fmt, ap);
    va_end(ap);

    draw_status_bar(a);
    mark_dirty(a, 0, a->win_h - STATUS_H, a->win_w, a->win_h);
    publish(a);

    for (int i = 0; i < 20 && a->shm->pub_x0 != SHM_DIRTY_EMPTY; i++)
        sleep_ms(5);
    sleep_ms(5);
}

/* ==================== 基础文件操作 ==================== */
static void format_size(uint32_t sz, char *out, int cap) {
    if (sz < 1024) snprintf(out, cap, "%u B", sz);
    else if (sz < 1024u * 1024) snprintf(out, cap, "%u KB", sz / 1024);
    else if (sz < 1024u * 1024 * 1024) snprintf(out, cap, "%u MB", sz / (1024 * 1024));
    else snprintf(out, cap, "%u GB", sz / (1024u * 1024 * 1024));
}

static int is_archive_name(const char *n) {
    const char *dot = strrchr(n, '.');
    if (!dot) return 0;
    return strcmp(dot, ".zip") == 0 || strcmp(dot, ".7z") == 0 ||
           strcmp(dot, ".rar") == 0 || strcmp(dot, ".tar") == 0 ||
           strcmp(dot, ".gz") == 0 || strcmp(dot, ".tgz") == 0 ||
           strcmp(dot, ".bz2") == 0 || strcmp(dot, ".xz") == 0;
}

static int is_zip_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    return dot && strcmp(dot, ".zip") == 0;
}

static int write_all(int fd, const void *buf, uint32_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0) {
        int r = fs_write(fd, p, n);
        if (r <= 0) return -1;
        p += r;
        n -= (uint32_t)r;
    }
    return 0;
}

static void make_dirs(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            char save = *p;
            *p = 0;
            if (path[0]) fs_mkdir(path);
            *p = save;
        }
    }
    if (path[0]) fs_mkdir(path);
}

static uint16_t read_u16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t read_u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

static uint32_t g_crc_tab[256];
static int      g_crc_ready = 0;

static void init_crc32(void) {
    if (g_crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t update_crc(uint32_t crc, const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        crc = g_crc_tab[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void zip_put_u16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void zip_put_u32(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

/* ==================== 极简 Deflate 压缩算法 (Fixed Huffman) ==================== */
static uint32_t bit_buf = 0;
static int bit_cnt = 0;
static uint8_t *out_ptr = NULL;
static int out_len = 0;

static void put_bits(int n, uint32_t val) {
    bit_buf |= (val & ((1 << n) - 1)) << bit_cnt;
    bit_cnt += n;
    while (bit_cnt >= 8) {
        out_ptr[out_len++] = bit_buf & 0xFF;
        bit_buf >>= 8;
        bit_cnt -= 8;
    }
}

static void huff_sym(int sym) {
    if (sym < 144) put_bits(8, 0x30 + sym);
    else if (sym < 256) put_bits(9, 0x190 + (sym - 144));
    else if (sym < 280) put_bits(7, sym - 256);
    else put_bits(8, 0xC0 + (sym - 280));
}

static void deflate_fixed(uint8_t *in, uint32_t len, uint8_t *out, uint32_t *out_sz) {
    static int length_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static int length_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static int dist_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static int dist_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

    out_ptr = out;
    out_len = 0;
    bit_buf = 0;
    bit_cnt = 0;
    
    put_bits(1, 1);
    put_bits(2, 1);

    uint32_t pos = 0;
    while (pos < len) {
        int best_len = 0, best_dist = 0;
        int max_len = len - pos;
        if (max_len > 258) max_len = 258;
        
        int start = pos > 32768 ? pos - 32768 : 0;
        for (int i = pos - 1; i >= start; i--) {
            int l = 0;
            while (l < max_len && in[pos + l] == in[i + l]) l++;
            if (l >= 3 && l > best_len) {
                best_len = l;
                best_dist = pos - i;
                if (l == max_len) break;
            }
        }
        
        if (best_len >= 3) {
            int lc = 0;
            while (lc < 28 && best_len >= length_base[lc + 1]) lc++;
            huff_sym(257 + lc);
            if (length_extra[lc]) put_bits(length_extra[lc], best_len - length_base[lc]);
            
            int dc = 0;
            while (dc < 29 && best_dist >= dist_base[dc + 1]) dc++;
            put_bits(5, dc);
            if (dist_extra[dc]) put_bits(dist_extra[dc], best_dist - dist_base[dc]);
            
            pos += best_len;
        } else {
            huff_sym(in[pos]);
            pos++;
        }
    }
    huff_sym(256);
    if (bit_cnt > 0) out_ptr[out_len++] = bit_buf & 0xFF;
    *out_sz = out_len;
}

/* ==================== ZIP 格式解析与解压 ==================== */
static int list_zip(const char *path, struct file_entry *out, int *out_n, int cap) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return -1;
    
    struct fstat_buf st;
    if (fs_fstat(fd, &st) < 0) { fs_close(fd); return -1; }
    uint32_t file_size = st.size;
    if (file_size < 22) { fs_close(fd); return -1; }
    
    uint32_t read_size = file_size < (65536 + 22) ? file_size : (65536 + 22);
    uint8_t *tail = malloc(read_size);
    if (!tail) { fs_close(fd); return -1; }
    
    fs_seek(fd, file_size - read_size);
    if (fs_read(fd, tail, read_size) != (int)read_size) { free(tail); fs_close(fd); return -1; }
    
    int eocd_pos = -1;
    for (int i = read_size - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i+1] == 0x4b && tail[i+2] == 0x05 && tail[i+3] == 0x06) {
            eocd_pos = i; break;
        }
    }
    if (eocd_pos == -1) { free(tail); fs_close(fd); return -1; }
    
    uint16_t total_entries = read_u16(tail + eocd_pos + 10);
    uint32_t cd_size = read_u32(tail + eocd_pos + 12);
    uint32_t cd_offset = read_u32(tail + eocd_pos + 16);
    free(tail);
    
    if (cd_offset + cd_size > file_size) { fs_close(fd); return -1; }
    
    uint8_t *cd = malloc(cd_size);
    if (!cd) { fs_close(fd); return -1; }
    fs_seek(fd, cd_offset);
    if (fs_read(fd, cd, cd_size) != (int)cd_size) { free(cd); fs_close(fd); return -1; }
    
    int n = 0;
    uint32_t pos = 0;
    for (int i = 0; i < total_entries && n < cap; i++) {
        if (pos + 46 > cd_size) break;
        if (read_u32(cd + pos) != 0x02014b50) break;
        
        uint32_t usize = read_u32(cd + pos + 24);
        uint16_t nlen = read_u16(cd + pos + 28);
        uint16_t elen = read_u16(cd + pos + 30);
        uint16_t clen = read_u16(cd + pos + 32);
        
        struct file_entry *e = &out[n++];
        int name_len = nlen;
        if (name_len >= 256) name_len = 255;
        memcpy(e->name, cd + pos + 46, name_len);
        e->name[name_len] = 0;
        e->is_dir = (usize == 0 && name_len > 0 && e->name[name_len - 1] == '/');
        e->size = usize;
        
        pos += 46 + nlen + elen + clen;
    }
    
    free(cd);
    fs_close(fd);
    *out_n = n;
    return 0;
}

static int extract_zip(const char *path) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return -1;
    
    struct fstat_buf st;
    if (fs_fstat(fd, &st) < 0) { fs_close(fd); return -1; }
    uint32_t file_size = st.size;
    
    uint32_t read_size = file_size < (65536 + 22) ? file_size : (65536 + 22);
    uint8_t *tail = malloc(read_size);
    fs_seek(fd, file_size - read_size);
    fs_read(fd, tail, read_size);
    
    int eocd_pos = -1;
    for (int i = read_size - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i+1] == 0x4b && tail[i+2] == 0x05 && tail[i+3] == 0x06) {
            eocd_pos = i; break;
        }
    }
    
    uint16_t total_entries = read_u16(tail + eocd_pos + 10);
    uint32_t cd_size = read_u32(tail + eocd_pos + 12);
    uint32_t cd_offset = read_u32(tail + eocd_pos + 16);
    free(tail);
    
    uint8_t *cd = malloc(cd_size);
    fs_seek(fd, cd_offset);
    fs_read(fd, cd, cd_size);
    
    uint8_t *buf = malloc(Z_BUF_SIZE);
    int ret = 0;
    uint32_t pos = 0;
    
    for (int i = 0; i < total_entries; i++) {
        if (pos + 46 > cd_size) break;
        if (read_u32(cd + pos) != 0x02014b50) break;
        
        uint16_t method = read_u16(cd + pos + 10);
        uint32_t csize = read_u32(cd + pos + 20);
        uint32_t usize = read_u32(cd + pos + 24);
        uint16_t nlen = read_u16(cd + pos + 28);
        uint16_t elen = read_u16(cd + pos + 30);
        uint16_t clen = read_u16(cd + pos + 32);
        uint32_t local_offset = read_u32(cd + pos + 42);
        
        char name[256];
        int name_len = nlen < 255 ? nlen : 255;
        memcpy(name, cd + pos + 46, name_len);
        name[name_len] = 0;
        
        int is_dir = (usize == 0 && name_len > 0 && name[name_len - 1] == '/');
        
        if (is_dir) {
            make_dirs(name);
        } else {
            char parent[256];
            strncpy(parent, name, 255);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = 0; make_dirs(parent); }
            
            uint8_t lh[30];
            fs_seek(fd, local_offset);
            fs_read(fd, lh, 30);
            uint16_t lnlen = read_u16(lh + 26);
            uint16_t lelen = read_u16(lh + 28);
            uint32_t data_offset = local_offset + 30 + lnlen + lelen;
            
            uint8_t *comp_data = malloc(csize);
            fs_seek(fd, data_offset);
            fs_read(fd, comp_data, csize);
            
            uint8_t *final_data = comp_data;
            
            if (method == 8) {
                final_data = malloc(usize);
                uint32_t in_pos = 0, out_pos = 0;
                uint32_t bit_buf = 0;
                int bit_cnt = 0;
                
                static const int len_base[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
                static const int len_extra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
                static const int dist_base[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
                static const int dist_extra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
                
                while (out_pos < usize) {
                    while (bit_cnt < 16 && in_pos < csize) { bit_buf |= comp_data[in_pos++] << bit_cnt; bit_cnt += 8; }
                    int b = bit_buf & 1; bit_buf >>= 1; bit_cnt--;
                    int type = bit_buf & 3; bit_buf >>= 2; bit_cnt -= 2;
                    if (type == 1 || type == 2) {
                        for (;;) {
                            int sym = 0, code = 0, bits = 0;
                            while (bits < 15) {
                                code |= (bit_buf & 1) << bits; bit_buf >>= 1; bit_cnt--; bits++;
                                if (bits == 7) { if (code >= 0x00 && code <= 0x17) { sym = 256 + code; break; } }
                                if (bits == 8) { if (code >= 0x30 && code <= 0xBF) { sym = code - 0x30; break; } if (code >= 0xC0 && code <= 0xC7) { sym = 280 + code - 0xC0; break; } }
                                if (bits == 9 && code >= 0x190 && code <= 0x1FF) { sym = 144 + code - 0x190; break; }
                            }
                            while (bit_cnt < 16 && in_pos < csize) { bit_buf |= comp_data[in_pos++] << bit_cnt; bit_cnt += 8; }
                            if (sym < 256) { final_data[out_pos++] = sym; }
                            else if (sym == 256) break;
                            else {
                                sym -= 257;
                                int l = len_base[sym] + (len_extra[sym] ? (bit_buf & ((1<<len_extra[sym])-1)) : 0);
                                bit_buf >>= len_extra[sym]; bit_cnt -= len_extra[sym];
                                while (bit_cnt < 16 && in_pos < csize) { bit_buf |= comp_data[in_pos++] << bit_cnt; bit_cnt += 8; }
                                int dist_code = bit_buf & 31; bit_buf >>= 5; bit_cnt -= 5;
                                int d = dist_base[dist_code] + (dist_extra[dist_code] ? (bit_buf & ((1<<dist_extra[dist_code])-1)) : 0);
                                bit_buf >>= dist_extra[dist_code]; bit_cnt -= dist_extra[dist_code];
                                for (int k = 0; k < l; k++) { final_data[out_pos] = final_data[out_pos - d]; out_pos++; }
                            }
                            if (b) break;
                        }
                    }
                }
            }
            
            int ofd = fs_open(name, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
            if (ofd >= 0) { write_all(ofd, final_data, usize); fs_close(ofd); }
            if (method == 8) free(final_data);
            free(comp_data);
        }
        pos += 46 + nlen + elen + clen;
    }
    
    free(cd); free(buf); fs_close(fd);
    return ret;
}

/* ==================== 7z 格式解析 ==================== */
static int u16_to_utf8(const UInt16 *s, size_t len, char *out, int cap) {
    int n = 0;
    for (size_t k = 0; k < len && s[k] && n < cap - 4; k++) {
        uint32_t c = s[k];
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = (char)(0xC0 | (c >> 6)); out[n++] = (char)(0x80 | (c & 0x3F)); }
        else { out[n++] = (char)(0xE0 | (c >> 12)); out[n++] = (char)(0x80 | ((c >> 6) & 0x3F)); out[n++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[n] = 0;
    return n;
}

static int list_7z(const char *path, struct file_entry *out, int *out_n, int cap) {
    CFileInStream archive_stream;
    if (InFile_Open(&archive_stream.file, path) != 0) return -1;
    FileInStream_CreateVTable(&archive_stream);
    archive_stream.wres = 0;
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte *)malloc(1u << 18);
    if (!look.buf) { File_Close(&archive_stream.file); return -1; }
    look.bufSize = 1u << 18; look.realStream = &archive_stream.vt; LookToRead2_INIT(&look);
    CrcGenerateTable();
    CSzArEx db; SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK) { SzArEx_Free(&db, &g_alloc); free(look.buf); File_Close(&archive_stream.file); return -1; }
    int n = 0;
    for (UInt32 i = 0; i < db.NumFiles && n < cap; i++) {
        int is_dir = SzArEx_IsDir(&db, i);
        size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
        if (len == 0 || len > 1024) continue;
        UInt16 *name16 = (UInt16 *)malloc(len * sizeof(UInt16));
        if (!name16) continue;
        SzArEx_GetFileNameUtf16(&db, i, name16);
        char name[1024];
        u16_to_utf8(name16, len, name, sizeof(name));
        free(name16);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
        struct file_entry *e = &out[n++];
        strncpy(e->name, name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        e->is_dir = is_dir;
        e->size = (uint32_t)SzArEx_GetFileSize(&db, i);
    }
    SzArEx_Free(&db, &g_alloc); free(look.buf); File_Close(&archive_stream.file);
    *out_n = n; return 0;
}

static int extract_7z(const char *path) {
    CFileInStream archive_stream;
    if (InFile_Open(&archive_stream.file, path) != 0) return -1;
    FileInStream_CreateVTable(&archive_stream); archive_stream.wres = 0;
    CLookToRead2 look;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte *)malloc(1u << 18);
    if (!look.buf) { File_Close(&archive_stream.file); return -1; }
    look.bufSize = 1u << 18; look.realStream = &archive_stream.vt; LookToRead2_INIT(&look);
    CrcGenerateTable();
    CSzArEx db; SzArEx_Init(&db);
    SRes res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_alloc);
    if (res != SZ_OK) { SzArEx_Free(&db, &g_alloc); free(look.buf); File_Close(&archive_stream.file); return -1; }
    UInt32 block_index = 0xFFFFFFFF;
    Byte *out_buf = NULL; size_t out_size = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        int is_dir = SzArEx_IsDir(&db, i);
        size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
        if (len == 0 || len > 1024) continue;
        UInt16 *name16 = (UInt16 *)malloc(len * sizeof(UInt16));
        if (!name16) continue;
        SzArEx_GetFileNameUtf16(&db, i, name16);
        char name[1024];
        u16_to_utf8(name16, len, name, sizeof(name));
        free(name16);
        for (char *p = name; *p; p++) if (*p == '\\') *p = '/';
        if (is_dir) { make_dirs(name); }
        else {
            size_t off = 0, out_len = 0;
            res = SzArEx_Extract(&db, &look.vt, i, &block_index, &out_buf, &out_size, &off, &out_len, &g_alloc, &g_alloc);
            if (res != SZ_OK) break;
            char parent[1024]; strncpy(parent, name, 1023);
            char *slash = strrchr(parent, '/');
            if (slash) { *slash = 0; make_dirs(parent); }
            int ofd = fs_open(name, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
            if (ofd < 0) { res = SZ_ERROR_FAIL; break; }
            if (write_all(ofd, out_buf + off, (uint32_t)out_len) < 0) { fs_close(ofd); res = SZ_ERROR_FAIL; break; }
            fs_close(ofd);
        }
    }
    if (out_buf) free(out_buf);
    SzArEx_Free(&db, &g_alloc); free(look.buf); File_Close(&archive_stream.file);
    return (res == SZ_OK) ? 0 : -1;
}

/* ==================== 统一分发 ==================== */
static int list_archive(const char *path, struct file_entry *out, int *out_n, int cap) {
    if (is_zip_ext(path)) return list_zip(path, out, out_n, cap);
    return list_7z(path, out, out_n, cap);
}
static int extract_archive(const char *path) {
    if (is_zip_ext(path)) return extract_zip(path);
    return extract_7z(path);
}

/* ==================== 创建压缩包 ==================== */
struct zip_entry { char name[256]; uint32_t crc; uint32_t csize; uint32_t usize; uint16_t method; uint32_t local_offset; };

static int create_zip_compressed(struct app *a, const char *zipname, char **files, int nfiles) {
    fs_unlink(zipname);
    int ffd = fs_open(zipname, FS_O_WRONLY | FS_O_CREAT | FS_O_TRUNC);
    if (ffd < 0) return -1;

    init_crc32();
    struct zip_entry *ents = malloc(sizeof(struct zip_entry) * (nfiles > 0 ? nfiles : 1));
    uint8_t *buf = malloc(Z_BUF_SIZE);
    uint8_t *out_buf = malloc(Z_BUF_SIZE * 2);
    if (!ents || !buf || !out_buf) {
        if (ents) free(ents); if (buf) free(buf); if (out_buf) free(out_buf);
        fs_close(ffd); fs_unlink(zipname); return -1;
    }

    int nent = 0; uint32_t offset = 0; uint8_t hdr[46];
    for (int i = 0; i < nfiles; i++) {
        set_progress(a, "Packing %d/%d: %s", i + 1, nfiles, files[i]);
        int fd = fs_open(files[i], FS_O_RDONLY);
        if (fd < 0) { sleep_ms(1); continue; }
        
        struct fstat_buf st;
        if (fs_fstat(fd, &st) < 0) { fs_close(fd); continue; }
        uint32_t usize = st.size;
        
        uint32_t crc = 0xFFFFFFFF;
        uint32_t csize = 0;
        uint16_t method = 8; // Deflate
        
        size_t nlen = strlen(files[i]); if (nlen > 255) nlen = 255;

        if (usize <= Z_BUF_SIZE) {
            /* 小文件：全量读取到内存，计算 CRC 并 Deflate 压缩 */
            fs_read(fd, buf, usize);
            crc = update_crc(crc, buf, usize) ^ 0xFFFFFFFF;
            deflate_fixed(buf, usize, out_buf, &csize);
            
            if (csize >= usize) { /* 如果压缩后变大，退化为 Stored */
                method = 0; csize = usize;
                memcpy(out_buf, buf, usize);
            }
            
            zip_put_u32(hdr + 0, 0x04034b50); zip_put_u16(hdr + 4, 20);
            zip_put_u16(hdr + 6, 0); zip_put_u16(hdr + 8, method);
            zip_put_u16(hdr + 10, 0); zip_put_u16(hdr + 12, 0x21);
            zip_put_u32(hdr + 14, crc); zip_put_u32(hdr + 18, csize); zip_put_u32(hdr + 22, usize);
            zip_put_u16(hdr + 26, (uint16_t)nlen); zip_put_u16(hdr + 28, 0);
            
            write_all(ffd, hdr, 30); write_all(ffd, files[i], (uint32_t)nlen); write_all(ffd, out_buf, csize);
            
            struct zip_entry *e = &ents[nent++];
            strncpy(e->name, files[i], 255); e->name[255] = 0;
            e->crc = crc; e->csize = csize; e->usize = usize; e->method = method; e->local_offset = offset;
            offset += 30 + (uint32_t)nlen + csize;
            
        } else {
            /* 大文件：采用流式 Stored 模式（Method 0），避免内存爆炸和 Deflate 卡死 */
            method = 0;
            uint32_t remain = usize;
            csize = usize;
            
            /* 第一遍：流式算 CRC */
            crc = 0xFFFFFFFF;
            while (remain > 0) {
                uint32_t chunk = remain < Z_BUF_SIZE ? remain : Z_BUF_SIZE;
                int r = fs_read(fd, buf, chunk);
                if (r <= 0) break;
                crc = update_crc(crc, buf, r);
                remain -= r;
                if ((remain & 0xFFFF) == 0) sleep_ms(1);
            }
            crc ^= 0xFFFFFFFF;
            
            fs_seek(fd, 0); /* 重置读取位置 */
            
            /* 写入 Local Header */
            zip_put_u32(hdr + 0, 0x04034b50); zip_put_u16(hdr + 4, 20);
            zip_put_u16(hdr + 6, 0); zip_put_u16(hdr + 8, method);
            zip_put_u16(hdr + 10, 0); zip_put_u16(hdr + 12, 0x21);
            zip_put_u32(hdr + 14, crc); zip_put_u32(hdr + 18, csize); zip_put_u32(hdr + 22, usize);
            zip_put_u16(hdr + 26, (uint16_t)nlen); zip_put_u16(hdr + 28, 0);
            write_all(ffd, hdr, 30); write_all(ffd, files[i], (uint32_t)nlen);
            
            /* 第二遍：流式写入数据 */
            remain = usize;
            while (remain > 0) {
                uint32_t chunk = remain < Z_BUF_SIZE ? remain : Z_BUF_SIZE;
                int r = fs_read(fd, buf, chunk);
                if (r <= 0) break;
                write_all(ffd, buf, (uint32_t)r);
                remain -= r;
                if ((remain & 0xFFFF) == 0) sleep_ms(1);
            }
            
            struct zip_entry *e = &ents[nent++];
            strncpy(e->name, files[i], 255); e->name[255] = 0;
            e->crc = crc; e->csize = csize; e->usize = usize; e->method = method; e->local_offset = offset;
            offset += 30 + (uint32_t)nlen + csize;
        }
        fs_close(fd);
        sleep_ms(1);
    }

    if (nent == 0) { free(ents); free(buf); free(out_buf); fs_close(ffd); fs_unlink(zipname); return -1; }

    uint32_t cd_start = offset, cd_size = 0;
    for (int i = 0; i < nent; i++) {
        struct zip_entry *e = &ents[i];
        size_t nlen = strlen(e->name); if (nlen > 255) nlen = 255;
        zip_put_u32(hdr + 0, 0x02014b50); zip_put_u16(hdr + 4, 20); zip_put_u16(hdr + 6, 20);
        zip_put_u16(hdr + 8, e->method); zip_put_u16(hdr + 10, 0); zip_put_u16(hdr + 12, 0); zip_put_u16(hdr + 14, 0x21);
        zip_put_u32(hdr + 16, e->crc); zip_put_u32(hdr + 20, e->csize); zip_put_u32(hdr + 24, e->usize);
        zip_put_u16(hdr + 28, (uint16_t)nlen); zip_put_u16(hdr + 30, 0); zip_put_u16(hdr + 32, 0);
        zip_put_u16(hdr + 34, 0); zip_put_u16(hdr + 36, 0); zip_put_u32(hdr + 38, 0); zip_put_u32(hdr + 42, e->local_offset);
        write_all(ffd, hdr, 46); write_all(ffd, e->name, (uint32_t)nlen);
        cd_size += 46 + (uint32_t)nlen;
    }

    zip_put_u32(hdr + 0, 0x06054b50); zip_put_u16(hdr + 4, 0); zip_put_u16(hdr + 6, 0);
    zip_put_u16(hdr + 8, (uint16_t)nent); zip_put_u16(hdr + 10, (uint16_t)nent);
    zip_put_u32(hdr + 12, cd_size); zip_put_u32(hdr + 16, cd_start); zip_put_u16(hdr + 20, 0);
    write_all(ffd, hdr, 22);

    fs_close(ffd); free(ents); free(buf); free(out_buf);
    return 0;
}

static int create_7z_cmd(struct app *a, const char *szname, char **files, int nfiles) {
    fs_unlink(szname);
    char *argv[512];
    int argc = 0;
    argv[argc++] = "/bin/7z";
    argv[argc++] = "a";
    argv[argc++] = "-y";
    argv[argc++] = (char*)szname;
    for (int i = 0; i < nfiles && argc < 510; i++) argv[argc++] = files[i];
    argv[argc] = NULL;
    
    set_progress(a, "Spawning /bin/7z a ...");
    int pid = spawn("/bin/7z", argv, NULL);
    if (pid < 0) return -1;
    
    int status = 0;
    wait(pid, &status, 0);
    return (status == 0) ? 0 : -1;
}

/* ==================== UI 与业务逻辑 ==================== */
static void do_create_zip(struct app *a, int is_7z) {
    if (a->in_archive) { set_status(a, "Exit archive view first"); return; }
    char name[256]; char *files[MAX_ENTRIES]; int nfiles = 0;
    
    if (a->sel >= 0 && a->sel < a->n_entries && !a->entries[a->sel].is_dir) {
        snprintf(name, sizeof(name), "%s.%s", a->entries[a->sel].name, is_7z ? "7z" : "zip");
        files[nfiles++] = a->entries[a->sel].name;
    } else {
        strcpy(name, is_7z ? "archive.7z" : "archive.zip");
        for (int i = 0; i < a->n_entries; i++) {
            if (a->entries[i].is_dir || strcmp(a->entries[i].name, "..") == 0 || strcmp(a->entries[i].name, name) == 0) continue;
            files[nfiles++] = a->entries[i].name;
        }
    }
    if (nfiles == 0) { set_status(a, "No files to compress"); return; }

    set_status(a, "Compressing...");
    redraw_all(a);
    publish(a); sleep_ms(20);

    int r = is_7z ? create_7z_cmd(a, name, files, nfiles) : create_zip_compressed(a, name, files, nfiles);
    read_dir(a);
    snprintf(a->status, sizeof(a->status), "%s: %s", r == 0 ? "Created" : "Failed", name);
}

static void draw_ui(struct app *a) {
    unsigned int *buf = a->pixels;
    int bw = a->win_w, bh = a->win_h;

    fill_rect(buf, bw, bh, 0, 0, bw, bh, COL_BG);

    fill_rect(buf, bw, bh, 0, 0, bw, TOOLBAR_H, COL_TOOLBAR);
    fill_rect(buf, bw, bh, 0, TOOLBAR_H - 1, bw, 1, COL_LINE);

    int extract_en = a->in_archive || (a->sel >= 0 && a->sel < a->n_entries && !a->entries[a->sel].is_dir);
    int compact_en = !a->in_archive;

    struct { const char *label; int x, w, action; int enabled; } btns[] = {
        { "Up",          8,  60, 0, !a->in_archive && strcmp(a->cwd, "/") != 0 },
        { "Refresh",    72,  80, 1, 1 },
        { "Extract",   156,  90, 2, extract_en },
        { "Compact",   250, 100, 3, compact_en },
        { "Exit",        0,  60, 4, 1 },
    };
    int n_btns = sizeof(btns) / sizeof(btns[0]);
    btns[4].x = bw - 60 - 8;

    for (int i = 0; i < n_btns; i++) {
        int bx = btns[i].x, by = 4, bw_ = btns[i].w, bh_ = 28;
        uint32_t bg = btns[i].enabled ? COL_BTN : COL_BTN_DIS;
        if (btns[i].action == 3 && a->menu_open) bg = COL_MENU_HL;
        
        fill_rect(buf, bw, bh, bx, by, bw_, bh_, bg);
        fill_rect(buf, bw, bh, bx, by, bw_, 1, COL_LINE);
        fill_rect(buf, bw, bh, bx, by + bh_ - 1, bw_, 1, COL_LINE);
        fill_rect(buf, bw, bh, bx, by, 1, bh_, COL_LINE);
        fill_rect(buf, bw, bh, bx + bw_ - 1, by, 1, bh_, COL_LINE);

        int len = strlen(btns[i].label);
        int tw = len * 8;
        draw_text(buf, bw, bh, bx + (bw_ - tw) / 2, by + (bh_ - 16) / 2,
                  bx + bw_, btns[i].label,
                  btns[i].enabled ? COL_BTN_FG : COL_BTN_FG_D);
    }

    fill_rect(buf, bw, bh, 0, TOOLBAR_H, bw, PATH_H, COL_PATH);
    const char *path = a->in_archive ? a->archive_path : a->cwd;
    draw_text(buf, bw, bh, 8, TOOLBAR_H + 3, bw - 8, path, COL_PATH_FG);

    int list_y = TOOLBAR_H + PATH_H;
    int list_h = bh - list_y - STATUS_H;
    int visible = list_h / ROW_H;

    for (int i = 0; i < visible; i++) {
        int idx = a->scroll + i;
        int ry = list_y + i * ROW_H;
        if (idx >= a->n_entries) break;

        struct file_entry *e = &a->entries[idx];
        uint32_t bgc = (idx == a->sel) ? COL_ROW_SEL : ((i & 1) ? COL_ROW_ODD : COL_ROW_EVEN);
        fill_rect(buf, bw, bh, 0, ry, bw, ROW_H, bgc);

        int icon_x = 8;
        if (e->is_dir) {
            fill_rect(buf, bw, bh, icon_x, ry + 4, 14, 12, 0xD4A020);
            fill_rect(buf, bw, bh, icon_x, ry + 2, 6, 3, 0xD4A020);
        } else {
            fill_rect(buf, bw, bh, icon_x + 1, ry + 3, 12, 14, 0x9090A8);
            fill_rect(buf, bw, bh, icon_x + 3, ry + 6, 8, 1, 0x606070);
            fill_rect(buf, bw, bh, icon_x + 3, ry + 9, 8, 1, 0x606070);
            fill_rect(buf, bw, bh, icon_x + 3, ry + 12, 6, 1, 0x606070);
        }

        draw_text(buf, bw, bh, 28, ry + 2, bw - 100, e->name, COL_ROW_FG);

        if (!e->is_dir) {
            char sz[32];
            format_size(e->size, sz, sizeof(sz));
            draw_text(buf, bw, bh, bw - 90, ry + 2, bw - 8, sz, COL_ROW_DIM);
        }
    }

    if (a->menu_open) {
        int mx = 250, my = TOOLBAR_H;
        fill_rect(buf, bw, bh, mx, my, 100, 56, COL_MENU_BG);
        fill_rect(buf, bw, bh, mx, my, 100, 1, COL_LINE);
        fill_rect(buf, bw, bh, mx, my + 55, 100, 1, COL_LINE);
        fill_rect(buf, bw, bh, mx, my, 1, 56, COL_LINE);
        fill_rect(buf, bw, bh, mx + 99, my, 1, 56, COL_LINE);
        
        draw_text(buf, bw, bh, mx + 10, my + 6, mx + 100, "Create ZIP", COL_BTN_FG);
        draw_text(buf, bw, bh, mx + 10, my + 34, mx + 100, "Create 7z", COL_BTN_FG);
    }

    draw_status_bar(a);
}

static void redraw_all(struct app *a) {
    draw_ui(a);
    mark_dirty(a, 0, 0, a->win_w, a->win_h);
}

static void read_dir(struct app *a) {
    a->in_archive = 0;
    a->n_entries = 0;
    a->sel = -1;
    a->scroll = 0;

    if (strcmp(a->cwd, "/") != 0 && a->n_entries < MAX_ENTRIES) {
        strcpy(a->entries[0].name, "..");
        a->entries[0].is_dir = 1;
        a->entries[0].size = 0;
        a->n_entries = 1;
    }

    int fd = fs_opendir(a->cwd);
    if (fd < 0) {
        snprintf(a->status, sizeof(a->status), "Cannot open: %s", a->cwd);
        return;
    }

    struct dirent ent;
    while (fs_readdir(fd, &ent) > 0 && a->n_entries < MAX_ENTRIES) {
        if (strcmp(ent.name, ".") == 0 || strcmp(ent.name, "..") == 0) continue;
        struct file_entry *e = &a->entries[a->n_entries++];
        strncpy(e->name, ent.name, sizeof(e->name) - 1);
        e->name[sizeof(e->name) - 1] = 0;
        e->is_dir = (ent.attributes & 0x10) ? 1 : 0;
        e->size = ent.size;
    }
    fs_closedir(fd);

    for (int i = 1; i < a->n_entries; i++) {
        for (int j = i; j > 0; j--) {
            struct file_entry *cur = &a->entries[j];
            struct file_entry *prev = &a->entries[j - 1];
            if (strcmp(prev->name, "..") == 0) break;
            int swap;
            if (cur->is_dir != prev->is_dir)
                swap = cur->is_dir > prev->is_dir;
            else
                swap = strcmp(cur->name, prev->name) < 0;
            if (!swap) break;
            struct file_entry tmp = *cur;
            *cur = *prev;
            *prev = tmp;
        }
    }
    snprintf(a->status, sizeof(a->status), "%s  -  %d entries", a->cwd, a->n_entries);
}

static void do_open_archive(struct app *a, const char *path) {
    struct file_entry *tmp = (struct file_entry *)malloc(
                                 sizeof(struct file_entry) * MAX_ENTRIES);
    if (!tmp) { set_status(a, "Out of memory"); redraw_all(a); return; }

    int n = 0;
    if (list_archive(path, tmp, &n, MAX_ENTRIES - 1) < 0) {
        free(tmp);
        set_status(a, "Failed to open archive");
        redraw_all(a);
        return;
    }

    a->entries[0].is_dir = 1;
    a->entries[0].size = 0;
    strcpy(a->entries[0].name, "..");
    memcpy(&a->entries[1], tmp, n * sizeof(struct file_entry));
    free(tmp);

    a->n_entries = n + 1;
    strncpy(a->archive_path, path, sizeof(a->archive_path) - 1);
    a->archive_path[sizeof(a->archive_path) - 1] = 0;
    a->in_archive = 1;
    a->sel = -1;
    a->scroll = 0;

    snprintf(a->status, sizeof(a->status), "%d entries in %s", n, path);
    redraw_all(a);
}

static void do_extract(struct app *a) {
    char path[256];
    path[0] = 0;

    if (a->in_archive) {
        strncpy(path, a->archive_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    } else if (a->sel >= 0 && a->sel < a->n_entries && !a->entries[a->sel].is_dir) {
        strncpy(path, a->entries[a->sel].name, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
    }

    if (!path[0]) {
        set_status(a, "Select an archive first");
        redraw_all(a);
        return;
    }

    set_status(a, "Extracting...");
    redraw_all(a);
    publish(a);
    sleep_ms(20);

    int r = extract_archive(path);
    if (r == 0) {
        read_dir(a);
        snprintf(a->status, sizeof(a->status), "Extracted %s", path);
    } else {
        read_dir(a);
        snprintf(a->status, sizeof(a->status), "Extract failed: %s", path);
    }
    redraw_all(a);
}

static void on_double_click(struct app *a, int idx) {
    if (idx < 0 || idx >= a->n_entries) return;
    struct file_entry *e = &a->entries[idx];

    if (strcmp(e->name, "..") == 0) {
        if (a->in_archive) {
            read_dir(a);
            redraw_all(a);
        } else if (chdir("..") == 0) {
            getcwd(a->cwd, sizeof(a->cwd));
            read_dir(a);
            redraw_all(a);
        } else {
            set_status(a, "Cannot go up");
            redraw_all(a);
        }
        return;
    }

    if (a->in_archive) return;

    if (e->is_dir) {
        if (chdir(e->name) == 0) {
            getcwd(a->cwd, sizeof(a->cwd));
            read_dir(a);
        } else {
            set_status(a, "Cannot enter directory");
        }
        redraw_all(a);
        return;
    }

    if (is_archive_name(e->name)) {
        do_open_archive(a, e->name);
        return;
    }

    snprintf(a->status, sizeof(a->status), "Selected: %s", e->name);
    redraw_all(a);
}

/* ==================== 事件处理 ==================== */
static void handle_event(struct app *a, struct wm_event *ev) {
    switch (ev->type) {
    case WM_EV_MOUSE_DOWN:
        if (ev->code != WM_BTN_LEFT) break;
        {
            int mx = ev->x, my = ev->y;

            if (a->menu_open) {
                if (mx >= 250 && mx < 350) {
                    if (my >= TOOLBAR_H && my < TOOLBAR_H + 28) {
                        a->menu_open = 0;
                        do_create_zip(a, 0);
                        redraw_all(a);
                        return;
                    } else if (my >= TOOLBAR_H + 28 && my < TOOLBAR_H + 56) {
                        a->menu_open = 0;
                        do_create_zip(a, 1);
                        redraw_all(a);
                        return;
                    }
                }
                a->menu_open = 0;
                redraw_all(a);
                return;
            }

            struct { int x, y, w, h, action; } btns[] = {
                { 8, 4, 60, 28, 0 }, { 72, 4, 80, 28, 1 }, { 156, 4, 90, 28, 2 },
                { 250, 4, 100, 28, 3 }, { a->win_w - 60 - 8, 4, 60, 28, 4 }
            };
            for (int i = 0; i < 5; i++) {
                if (mx >= btns[i].x && mx < btns[i].x + btns[i].w &&
                    my >= btns[i].y && my < btns[i].y + btns[i].h) {
                    switch (btns[i].action) {
                    case 0: /* Up */
                        if (a->in_archive) { read_dir(a); redraw_all(a); }
                        else if (chdir("..") == 0) { getcwd(a->cwd, sizeof(a->cwd)); read_dir(a); redraw_all(a); }
                        else { set_status(a, "Cannot go up"); redraw_all(a); }
                        break;
                    case 1: /* Refresh */
                        if (!a->in_archive) read_dir(a);
                        redraw_all(a); break;
                    case 2: do_extract(a); break;
                    case 3: /* Compact */
                        a->menu_open = 1;
                        redraw_all(a);
                        break;
                    case 4: _exit(0); break;
                    }
                    return;
                }
            }

            int list_y = TOOLBAR_H + PATH_H;
            int list_h = a->win_h - list_y - STATUS_H;
            if (my >= list_y && my < list_y + list_h) {
                int row = (my - list_y) / ROW_H;
                int idx = a->scroll + row;
                if (idx >= 0 && idx < a->n_entries) {
                    int is_dbl = (idx == a->last_click_idx && a->tick - a->last_click_tick < 50);
                    if (is_dbl) { a->last_click_idx = -1; on_double_click(a, idx); }
                    else { a->sel = idx; a->last_click_idx = idx; a->last_click_tick = a->tick; redraw_all(a); }
                }
            }
        }
        break;
    case WM_EV_WHEEL:
        if (a->menu_open) break;
        a->scroll -= ev->code / 120;
        if (a->scroll < 0) a->scroll = 0;
        { int maxscroll = a->n_entries - (a->win_h - TOOLBAR_H - PATH_H - STATUS_H) / ROW_H;
          if (maxscroll < 0) maxscroll = 0; if (a->scroll > maxscroll) a->scroll = maxscroll; }
        redraw_all(a);
        break;
    case WM_EV_CLOSE_REQUEST: _exit(0); break;
    case WM_EV_KEY:
        if (ev->code == 27) { if (a->menu_open) { a->menu_open = 0; redraw_all(a); } else _exit(0); }
        else if (ev->code == 0x151) { if (a->sel < a->n_entries - 1) a->sel++; redraw_all(a); }
        else if (ev->code == 0x150) { if (a->sel > 0) a->sel--; redraw_all(a); }
        else if (ev->code == '\n' && a->sel >= 0) on_double_click(a, a->sel);
        break;
    }
}

/* ==================== main ==================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    struct app a; memset(&a, 0, sizeof(a));
    a.sel = -1; a.last_click_idx = -1; a.win_w = WIN_W; a.win_h = WIN_H;
    if (getcwd(a.cwd, sizeof(a.cwd)) < 0) strcpy(a.cwd, "/");

    int pid = getpid();
    char shm_name[16], ev_name[16];
    snprintf(shm_name, sizeof(shm_name), "shm%d", pid);
    snprintf(ev_name,  sizeof(ev_name),  "ev%d",  pid);

    int shm_size = (int)shm_total_size(a.win_w, a.win_h);
    a.shm_id = shm_open(shm_name, (unsigned int)shm_size, 0);
    unsigned int addr = mmap(0, (unsigned int)shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, a.shm_id, 0);
    a.shm = (struct shm_hdr *)addr;
    a.pixels = (unsigned int *)(addr + SHM_HDR_SIZE);
    a.shm->counter = 0; a.shm->cur_x0 = SHM_DIRTY_EMPTY;
    a.shm->cur_y0 = 0; a.shm->cur_x1 = 0; a.shm->cur_y1 = 0;
    a.shm->pub_x0 = SHM_DIRTY_EMPTY; a.shm->pub_y0 = 0; a.shm->pub_x1 = 0; a.shm->pub_y1 = 0;

    int fds[2]; pipe_unlink(ev_name);
    pipe_named(ev_name, fds); a.ev_fd = fds[0]; ipc_close(fds[1]);

    wm_font_load();
    a.wm_fd = pipe_open("wm-inbox", 1);

    struct wm_request req; memset(&req, 0, sizeof(req));
    req.magic = WM_MAGIC; req.type  = WM_REQ_CREATE; req.kind  = WM_KIND_NORMAL;
    req.x = 120; req.y = 80; req.w = (uint32_t)a.win_w; req.h = (uint32_t)a.win_h;
    req.min_w = (uint32_t)a.win_w; req.min_h = (uint32_t)a.win_h;
    req.max_w = (uint32_t)a.win_w; req.max_h = (uint32_t)a.win_h;
    req.owner_pid = (uint32_t)pid;
    strncpy(req.shm_name, shm_name, sizeof(req.shm_name) - 1);
    strncpy(req.ev_name,  ev_name,  sizeof(req.ev_name)  - 1);
    strcpy(req.title, "7-Zip");
    pipe_write(a.wm_fd, &req, sizeof(req));

    read_dir(&a);
    set_status(&a, "Ready");
    redraw_all(&a); publish(&a);

    for (;;) {
        a.tick++;
        struct wm_event ev;
        while (pipe_avail(a.ev_fd) >= (int)sizeof(ev)) {
            if (pipe_read(a.ev_fd, &ev, sizeof(ev)) != (int)sizeof(ev)) break;
            handle_event(&a, &ev);
        }
        publish(&a);
        sleep_ms(10);
    }
    return 0;
}