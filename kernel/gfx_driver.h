#ifndef GFX_DRIVER_H
#define GFX_DRIVER_H
#include <stdint.h>

#define GFX_DRV_VBE     0
#define GFX_DRV_BOCHS   1
#define GFX_DRV_VMWARE  2

typedef struct {
    int driver_type;
    uint32_t fb_phys;
    uint32_t fb_virt;
    int width;
    int height;
    int bpp;
    int pitch;
    int vram_size;
} gfx_mode_t;

int gfx_init(void);
int gfx_set_mode(int w, int h, int bpp);
gfx_mode_t* gfx_get_mode(void);
uint32_t gfx_get_fb(void);
int gfx_get_width(void);
int gfx_get_height(void);
int gfx_get_pitch(void);
const char* gfx_get_driver_name(void);
int gfx_get_driver_type(void);

#endif
