#ifndef GPU_SVGA3D_H
#define GPU_SVGA3D_H
#include "gfx_3d.h"
#include <stdint.h>

int gpu_svga3d_probe(void);
int gpu_svga3d_init(void);
int gpu_svga3d_set_mode(int w, int h, int bpp);
int gpu_svga3d_is_available(void);
int gpu_svga3d_is_gmr_ready(void);

uint32_t gpu_svga3d_get_fb(void);
uint32_t gpu_svga3d_get_width(void);
uint32_t gpu_svga3d_get_height(void);
uint32_t gpu_svga3d_get_pitch(void);

void gpu_svga3d_register_fb_pages(const uint32_t* phys_pages, int count);
void gpu_svga3d_setup_gmr_fb(void);

void gpu_svga3d_update_rect(int x, int y, int w, int h);
void gpu_svga3d_fillrect(int x, int y, int w, int h, uint32_t color);
void gpu_svga3d_blit_gmr_to_screen(int sx, int sy, int dx, int dy, int w, int h);
void gpu_svga3d_blit_all(int w, int h);
void gpu_svga3d_sync(void);

void gpu_svga3d_draw_triangle(fixed_t* v0, fixed_t* v1, fixed_t* v2, uint32_t color);

#endif