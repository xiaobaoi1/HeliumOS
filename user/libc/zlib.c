#include "zlib.h"
#include "inflate.h"
#include "stdlib.h"
#include "string.h"
#include "syscall.h"

struct zstate {
    uint8_t *out_buf;
    int      out_len;
    int      out_pos;
    int      done;
};

int inflateInit2(z_stream *strm, int windowBits) {
    (void)windowBits;
    if (!strm) return Z_STREAM_ERROR;

    struct zstate *st = (struct zstate*)malloc(sizeof(*st));
    if (!st) return Z_MEM_ERROR;
    memset(st, 0, sizeof(*st));

    strm->state     = st;
    strm->total_in  = 0;
    strm->total_out = 0;
    return Z_OK;
}

int inflate(z_stream *strm, int flush) {
    (void)flush;
    if (!strm || !strm->state) return Z_STREAM_ERROR;
    struct zstate *st = (struct zstate*)strm->state;

    /* 第一次调用：把全部输入解压到内部 buffer。
     * NetSurf 的 hashtable 用法是完整内存块一次性喂进来，
     * 所以这里可以一次性解压。 */
    if (!st->done) {
        int max = 1 << 20;   /* 1MB 上限，mime.types 远小于此 */
        st->out_buf = (uint8_t*)malloc(max);
        if (!st->out_buf) return Z_MEM_ERROR;

        /* 先试 gzip（windowBits=32+MAX_WBITS 对应 gzip） */
        int n = gzip_decompress(strm->next_in, (int)strm->avail_in,
                                st->out_buf, max);
        if (n < 0) {
            /* 再试 raw deflate */
            int raw_len = 0;
            if (inflate_raw(strm->next_in, (int)strm->avail_in,
                            st->out_buf, max, &raw_len) == 0) {
                n = raw_len;
            } else {
                free(st->out_buf);
                st->out_buf = NULL;
                return Z_DATA_ERROR;
            }
        }
        st->out_len = n;
        strm->total_in += strm->avail_in;
        strm->avail_in  = 0;
        st->done = 1;
    }

    /* 从内部 buffer 拷到 next_out */
    int remain = st->out_len - st->out_pos;
    if (remain <= 0) return Z_STREAM_END;

    int n = remain < (int)strm->avail_out ? remain : (int)strm->avail_out;
    if (n <= 0) return Z_BUF_ERROR;

    memcpy(strm->next_out, st->out_buf + st->out_pos, n);
    st->out_pos      += n;
    strm->next_out   += n;
    strm->avail_out  -= n;
    strm->total_out  += n;

    if (st->out_pos >= st->out_len) return Z_STREAM_END;
    return Z_OK;
}

int inflateEnd(z_stream *strm) {
    if (!strm || !strm->state) return Z_OK;
    struct zstate *st = (struct zstate*)strm->state;
    if (st->out_buf) free(st->out_buf);
    free(st);
    strm->state = NULL;
    return Z_OK;
}

/* gzFile: 一次性读取整个文件，如果是 gzip 则解压，
 * 然后 gzgets 从内存逐行返回。 */
struct gzFile_s {
    uint8_t *data;
    int      len;
    int      pos;
};

gzFile gzopen(const char *path, const char *mode) {
    (void)mode;
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return NULL;

    int cap = 4096, total = 0;
    uint8_t *raw = (uint8_t*)malloc(cap);
    if (!raw) { fs_close(fd); return NULL; }

    while (1) {
        if (total == cap) {
            int ncap = cap * 2;
            uint8_t *nraw = (uint8_t*)realloc(raw, ncap);
            if (!nraw) { free(raw); fs_close(fd); return NULL; }
            raw = nraw; cap = ncap;
        }
        int n = fs_read(fd, raw + total, cap - total);
        if (n <= 0) break;
        total += n;
    }
    fs_close(fd);

    if (total == 0) { free(raw); return NULL; }

    /* 尝试 gzip 解压，失败则当明文 */
    uint8_t *decoded = NULL;
    int dec_len = 0;

    if (total >= 2 && raw[0] == 0x1f && raw[1] == 0x8b) {
        int max = total * 8;
        if (max < 65536) max = 65536;
        uint8_t *tmp = (uint8_t*)malloc(max);
        if (tmp) {
            int n = gzip_decompress(raw, total, tmp, max);
            if (n > 0) { decoded = tmp; dec_len = n; }
            else free(tmp);
        }
    }

    if (!decoded) { decoded = raw; dec_len = total; }
    else free(raw);

    struct gzFile_s *f = (struct gzFile_s*)malloc(sizeof(*f));
    if (!f) { free(decoded); return NULL; }
    f->data = decoded;
    f->len  = dec_len;
    f->pos  = 0;
    return f;
}

char *gzgets(gzFile file, char *buf, int len) {
    if (!file || !buf || len <= 1) return NULL;
    if (file->pos >= file->len) return NULL;

    int i = 0;
    while (i < len - 1 && file->pos < file->len) {
        char c = (char)file->data[file->pos++];
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = '\0';
    return buf;
}

int gzclose(gzFile file) {
    if (!file) return -1;
    if (file->data) free(file->data);
    free(file);
    return 0;
}