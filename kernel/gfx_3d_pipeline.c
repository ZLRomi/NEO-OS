#include "gfx_3d_pipeline.h"
#include "gpu_svga3d.h"
#include "gfx_3d.h"
#include "printk.h"

static int is_hw = 0;

int gfx_3d_pipeline_init(void) {
    if (gpu_svga3d_is_available()) {
        is_hw = 1;
        printk("3D pipeline: hardware acceleration (SVGA3D)\n");
        return 1;
    }
    if (gpu_svga3d_probe() && gpu_svga3d_init()) {
        is_hw = 1;
        printk("3D pipeline: hardware acceleration (SVGA3D)\n");
    } else {
        is_hw = 0;
        gfx_3d_init();
        printk("3D pipeline: software rendering\n");
    }
    return 1;
}

int gfx_3d_pipeline_is_hw(void) {
    return is_hw;
}
