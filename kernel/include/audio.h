#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

int audio_init(void);

/* 音量 0..100（0 = 静音） */
void audio_set_volume(int percent);
int  audio_get_volume(void);

/* 播放用户空间 PCM：44.1kHz / 16-bit / 立体声。
 * uaddr 为用户指针，len 为字节数（上限 1MB）。阻塞到播放完成。 */
int audio_play_user(uint32_t uaddr, uint32_t len);

#endif
