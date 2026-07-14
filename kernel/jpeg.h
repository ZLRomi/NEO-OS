#ifndef JPEG_H
#define JPEG_H

#include <stdint.h>

int jpeg_decode(const char* path, uint32_t** out_pixels, int* out_w, int* out_h);

#endif
