#include "gfx_driver.h"
#include "memory.h"
#include "pci.h"
#include "gpu_svga3d.h"
#include "printk.h"
#include "serial.h"
#include <stdint.h>

static gfx_mode_t current_mode;
static int driver_type = GFX_DRV_VBE;
static int gfx_inited = 0;

int gfx_init(void) {
    pci_dev_t dev;
    if (pci_find_device(0x1234, 0x1111, &dev)) {
        driver_type = GFX_DRV_BOCHS;
        printk("gfx: Bochs/QEMU BGA detected\n");
    } else if (pci_find_device(0x15AD, 0x0405, &dev)) {
        driver_type = GFX_DRV_VMWARE;
        printk("gfx: VMware SVGA-II detected\n");
    } else {
        driver_type = GFX_DRV_VBE;
        printk("gfx: generic VBE fallback\n");
    }
    gfx_inited = 1;
    return 1;
}

int gfx_set_mode(int w, int h, int bpp) {
    if (!gfx_inited) gfx_init();

    if (current_mode.fb_virt) {
        if (current_mode.width == w && current_mode.height == h && current_mode.bpp == bpp) {
            return 1;
        }
        if (driver_type != GFX_DRV_VMWARE) {
            vbe_disable();
        }
    }

    if (driver_type == GFX_DRV_VMWARE) {
        if (!gpu_svga3d_init()) {
            serial_write("gfx: SVGA3D init failed, falling back to VBE\n");
            driver_type = GFX_DRV_VBE;
        }
    }

    if (driver_type == GFX_DRV_VMWARE) {
        if (!gpu_svga3d_set_mode(w, h, bpp)) {
            serial_write("gfx: SVGA3D set_mode failed\n");
            return 0;
        }
        current_mode.driver_type = driver_type;
        current_mode.fb_phys = 0;
        current_mode.fb_virt = gpu_svga3d_get_fb();
        current_mode.width = (int)gpu_svga3d_get_width();
        current_mode.height = (int)gpu_svga3d_get_height();
        current_mode.pitch = (int)gpu_svga3d_get_pitch();
        current_mode.bpp = (current_mode.width > 0) ? (current_mode.pitch * 8) / current_mode.width : bpp;
        current_mode.vram_size = current_mode.height * current_mode.pitch;
    } else {
        if (!vbe_init((uint32_t)w, (uint32_t)h, (uint32_t)bpp)) {
            serial_write("gfx: vbe_init failed\n");
            return 0;
        }
        current_mode.driver_type = driver_type;
        current_mode.fb_phys = 0;
        current_mode.fb_virt = vbe_get_fb();
        current_mode.width = (int)vbe_get_width();
        current_mode.height = (int)vbe_get_height();
        current_mode.pitch = (int)vbe_get_pitch();
        current_mode.bpp = (current_mode.width > 0) ? (current_mode.pitch * 8) / current_mode.width : bpp;
        current_mode.vram_size = current_mode.height * current_mode.pitch;
    }

    printk("gfx: %dx%dx%d via %s\n",
           current_mode.width, current_mode.height, current_mode.bpp,
           gfx_get_driver_name());
    return 1;
}

gfx_mode_t* gfx_get_mode(void) {
    return &current_mode;
}

uint32_t gfx_get_fb(void) {
    return current_mode.fb_virt;
}

int gfx_get_width(void) {
    return current_mode.width;
}

int gfx_get_height(void) {
    return current_mode.height;
}

int gfx_get_pitch(void) {
    return current_mode.pitch;
}

const char* gfx_get_driver_name(void) {
    switch (driver_type) {
        case GFX_DRV_BOCHS:  return "Bochs BGA";
        case GFX_DRV_VMWARE: return "VMware SVGA-II";
        default:             return "VBE";
    }
}

int gfx_get_driver_type(void) {
    return driver_type;
}
