/* media.c - WAV 音频播放器
 *
 * 用法:
 *   media <file.wav>        播放 WAV（自动转成 48kHz / 16-bit / stereo）
 *   media -v <0..100>       设置系统音量
 *   media                   显示用法和当前音量
 *
 * 音频输出走 AC'97 bus-master DMA；内核接口见 SYS_AUDIO_*。
 */
#include "syscall.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include <stdint.h>

#define OUT_RATE       44100
#define PLAY_CHUNK     (1024 * 1024)
#define MAX_FILE_BYTES (64u * 1024u * 1024u)

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int tag_eq(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

struct wav_info {
    int channels;
    int bits;
    int rate;
    const uint8_t *data;
    uint32_t data_size;
};

static int wav_parse(const uint8_t *buf, uint32_t size, struct wav_info *wi) {
    if (size < 12 || !tag_eq(buf, "RIFF") || !tag_eq(buf + 8, "WAVE"))
        return 0;

    memset(wi, 0, sizeof(*wi));
    uint32_t off = 12;
    while (off + 8 <= size) {
        const uint8_t *ck = buf + off;
        uint32_t csz = rd32(ck + 4);
        uint32_t body = off + 8;
        uint32_t avail = size - body;
        if (csz > avail) csz = avail;

        if (tag_eq(ck, "fmt ") && csz >= 16) {
            int fmt = rd16(buf + body);
            wi->channels = rd16(buf + body + 2);
            wi->rate = (int)rd32(buf + body + 4);
            wi->bits = rd16(buf + body + 14);
            /* 1 = PCM，0xFFFE = WAVE_FORMAT_EXTENSIBLE（也按 PCM 处理） */
            if (fmt != 1 && fmt != 0xFFFE) return 0;
        } else if (tag_eq(ck, "data")) {
            wi->data = buf + body;
            wi->data_size = csz;
        }

        off = body + csz;
        if (csz & 1) off++;      /* RIFF chunk 2 字节对齐 */
    }

    if (!wi->data || !wi->data_size) return 0;
    if (wi->channels < 1 || wi->channels > 2) return 0;
    if (wi->bits != 8 && wi->bits != 16 && wi->bits != 24 && wi->bits != 32)
        return 0;
    if (wi->rate < 4000 || wi->rate > 192000) return 0;
    return 1;
}

static int16_t sample_at(const uint8_t *p, int bits) {
    switch (bits) {
    case 8:  return (int16_t)(((int)p[0] - 128) << 8);
    case 16: return (int16_t)rd16(p);
    case 24: return (int16_t)((int16_t)(p[1] | (p[2] << 8)));
    default: return (int16_t)(rd32(p) >> 16);
    }
}

/* 转换为 48kHz / 16-bit / stereo。返回字节数，失败返回 0。 */
static uint32_t wav_convert(const struct wav_info *wi, int16_t **out) {
    uint32_t sample_bytes = (uint32_t)wi->bits / 8;
    uint32_t frame_bytes = (uint32_t)wi->channels * sample_bytes;
    uint32_t frames = wi->data_size / frame_bytes;
    if (frames == 0) return 0;

    uint32_t rate = (uint32_t)wi->rate;
    uint32_t out_frames = frames / rate * OUT_RATE +
                          (frames % rate) * OUT_RATE / rate;
    if (out_frames == 0) out_frames = 1;

    uint32_t out_bytes = out_frames * 4u;
    if (out_bytes > MAX_FILE_BYTES) return 0;

    int16_t *buf = (int16_t*)malloc(out_bytes);
    if (!buf) return 0;

    /* 16.16 定点步进，避免 64 位除法依赖 libgcc。 */
    uint32_t step = ((rate / OUT_RATE) << 16) +
                    ((rate % OUT_RATE) << 16) / OUT_RATE;
    uint32_t src_fp = 0;
    for (uint32_t i = 0; i < out_frames; i++) {
        uint32_t idx = src_fp >> 16;
        if (idx >= frames) idx = frames - 1;
        const uint8_t *fp = wi->data + (uint32_t)idx * frame_bytes;
        int16_t l = sample_at(fp, wi->bits);
        int16_t r = (wi->channels > 1)
                        ? sample_at(fp + sample_bytes, wi->bits) : l;
        buf[i * 2] = l;
        buf[i * 2 + 1] = r;
        src_fp += step;
    }

    *out = buf;
    return out_bytes;
}

static int read_file(const char *path, uint8_t **out, uint32_t *out_size) {
    int fd = fs_open(path, FS_O_RDONLY);
    if (fd < 0) return -1;

    struct fstat_buf st;
    memset(&st, 0, sizeof(st));
    if (fs_fstat(fd, &st) < 0 || st.size < 44 || st.size > MAX_FILE_BYTES) {
        fs_close(fd);
        return -2;
    }

    uint8_t *buf = (uint8_t*)malloc(st.size);
    if (!buf) { fs_close(fd); return -3; }

    uint32_t done = 0;
    while (done < st.size) {
        int n = fs_read(fd, buf + done, st.size - done);
        if (n < 0) { free(buf); fs_close(fd); return -4; }
        if (n == 0) break;
        done += (uint32_t)n;
    }
    fs_close(fd);

    if (done < 44) { free(buf); return -5; }
    *out = buf;
    *out_size = done;
    return 0;
}

static int play_file(const char *path) {
    uint8_t *file = NULL;
    uint32_t size = 0;
    int rc = read_file(path, &file, &size);
    if (rc < 0) {
        printf("media: cannot read '%s' (%d)\n", path, rc);
        return 1;
    }

    struct wav_info wi;
    if (!wav_parse(file, size, &wi)) {
        printf("media: '%s' is not a supported WAV file\n", path);
        free(file);
        return 1;
    }

    uint32_t ms = 0;
    {
        uint32_t sample_bytes = (uint32_t)wi.bits / 8;
        uint32_t frame_bytes = (uint32_t)wi.channels * sample_bytes;
        uint32_t frames = wi.data_size / frame_bytes;
        ms = frames / (uint32_t)wi.rate * 1000u +
             (frames % (uint32_t)wi.rate) * 1000u / (uint32_t)wi.rate;
    }

    printf("media: %s\n", path);
    printf("  %d Hz, %d ch, %d-bit, %u ms, volume %d%%\n",
           wi.rate, wi.channels, wi.bits, ms, audio_get_volume());

    int16_t *pcm = NULL;
    uint32_t bytes = wav_convert(&wi, &pcm);
    free(file);
    if (!pcm || bytes == 0) {
        printf("media: conversion failed\n");
        if (pcm) free(pcm);
        return 1;
    }

    printf("media: playing...\n");
    int ret = 0;
    for (uint32_t off = 0; off < bytes; off += PLAY_CHUNK) {
        uint32_t n = bytes - off;
        if (n > PLAY_CHUNK) n = PLAY_CHUNK;
        int r = audio_play((uint8_t*)pcm + off, n);
        if (r < 0) {
            printf("media: audio_play failed (%d)\n", r);
            ret = 1;
            break;
        }
    }
    free(pcm);
    if (ret == 0) printf("media: done\n");
    return ret;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "-v") == 0) {
        int v = atoi(argv[2]);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (audio_set_volume(v) < 0) {
            printf("media: no audio device\n");
            return 1;
        }
        printf("media: volume = %d%%\n", audio_get_volume());
        return 0;
    }

    if (argc >= 2 && argv[1][0] != '-')
        return play_file(argv[1]);

    printf("media - WAV player\n");
    printf("  media <file.wav>     play a WAV file\n");
    printf("  media -v <0..100>    set system volume\n");
    printf("  current volume: %d%%\n", audio_get_volume());
    return 0;
}
