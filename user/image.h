#ifndef USER_IMAGE_H
#define USER_IMAGE_H

#include <stdint.h>

struct image {
    int w, h;
    uint32_t *pixels; /* 0xAARRGGBB */
};

struct image_frame {
    int delay_ms;       /* 建议显示时长；<=0 视为 100ms */
    uint32_t *pixels;   /* 0xAARRGGBB，宽高同 anim.w/h */
};

struct image_anim {
    int w, h;
    int count;
    struct image_frame *frames;
};

/* 静态读取（PNG / JPEG / BMP / GIF 首帧） */
int  image_load(struct image *img, const char *path);
void image_free(struct image *img);

/* 动画读取：GIF 多帧；PNG/JPEG/BMP 返回单帧（delay_ms=0） */
int  image_load_anim(struct image_anim *anim, const char *path);
void image_anim_free(struct image_anim *anim);

#endif