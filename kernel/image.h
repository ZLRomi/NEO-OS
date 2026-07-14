#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

int image_load(const char* path, uint32_t** out_pixels, int* out_w, int* out_h);

#endif
