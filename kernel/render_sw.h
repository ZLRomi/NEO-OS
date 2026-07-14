#ifndef RENDER_SW_H
#define RENDER_SW_H
#include "gfx_3d.h"
#include <stdint.h>

typedef struct {
    vec3f pos;
    fixed_t w;
    uint32_t color;
} vertex3d;

void render_triangle(vertex3d v0, vertex3d v1, vertex3d v2);
void render_clear(void);

#endif
