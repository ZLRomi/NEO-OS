#ifndef BMP_H
#define BMP_H

#include <stdint.h>

int bmp_decode(const char* path, uint32_t** out_pixels, int* out_w, int* out_h);

#endif
