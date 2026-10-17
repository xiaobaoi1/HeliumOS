#ifndef USER_SHM_LAYOUT_H
#define USER_SHM_LAYOUT_H

#include <stdint.h>

/*
 * 窗口 shm 布局（term/app 与 WM 共享）：
 *
 *   offset 0:  uint64_t counter          每次发布后 ++
 *   offset 8:  uint32_t cur_x0..cur_y1   term 正在累积的脏区（WM 不读）
 *   offset 24: uint32_t pub_x0..pub_y1   已发布脏区（WM 读）
 *   offset 40: uint32_t pixels[w*h]      像素（32bpp）
 *
 * 空脏区：x0 == 0xFFFFFFFF 或 x1 <= x0。
 *
 * 协议：
 *   term: mark_dirty 更新 cur；批末若 pub 空，cur→pub 后 counter++，清 cur
 *         WM 未消费 pub 时不发布，cur 继续累积（批次合并，不丢更新）
 *   WM:   counter 变化 → 读 pub → blit → 清 pub（x0=0xFFFFFFFF）
 */

#define SHM_HDR_SIZE  40
#define SHM_DIRTY_EMPTY  0xFFFFFFFFu

struct shm_hdr {
    volatile uint64_t counter;
    volatile uint32_t cur_x0;
    volatile uint32_t cur_y0;
    volatile uint32_t cur_x1;
    volatile uint32_t cur_y1;
    volatile uint32_t pub_x0;
    volatile uint32_t pub_y0;
    volatile uint32_t pub_x1;
    volatile uint32_t pub_y1;
};

static inline uint32_t shm_total_size(int w, int h) {
    return SHM_HDR_SIZE + (uint32_t)w * (uint32_t)h * 4;
}

#endif