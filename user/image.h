#ifndef USER_IMAGE_H
#define USER_IMAGE_H

#include <stdint.h>

struct image {
    int w, h;
    uint32_t *pixels; /* 0xAARRGGBB */
};

int image_load(struct image *img, const char *path);
void image_free(struct image *img);

#endif
