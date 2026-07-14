#include "gpu_svga3d.h"
#include "gfx_driver.h"
#include "pci.h"
#include "memory.h"
#include "heap.h"
#include "printk.h"
#include "serial.h"
#include "io.h"
#include <stdint.h>

#define PAGE_SIZE 4096

static void serial_hex(uint32_t num) {
    const char* hex = "0123456789ABCDEF";
    int started = 0;
    for (int i = 28; i >= 0; i -= 4) {
        uint32_t nibble = (num >> i) & 0xF;
        if (nibble) started = 1;
        if (started || i == 0) serial_putchar(hex[nibble]);
    }
}

/* VMware SVGA-II register/port constants */
#define SVGA_IO_INDEX        0
#define SVGA_IO_VALUE        1
#define SVGA_IO_BIOS         2
#define SVGA_IO_IRQSTATUS    8

#define SVGA_REG_ID                    0
#define SVGA_REG_ENABLE                1
#define SVGA_REG_WIDTH                 2
#define SVGA_REG_HEIGHT                3
#define SVGA_REG_MAX_WIDTH             4
#define SVGA_REG_MAX_HEIGHT            5
#define SVGA_REG_DEPTH                 6
#define SVGA_REG_BITS_PER_PIXEL        7
#define SVGA_REG_BYTES_PER_LINE        12
#define SVGA_REG_FB_START             13
#define SVGA_REG_FB_OFFSET            14
#define SVGA_REG_VRAM_SIZE            15
#define SVGA_REG_FB_SIZE              16
#define SVGA_REG_CAPABILITIES         17
#define SVGA_REG_MEM_START            18
#define SVGA_REG_MEM_SIZE             19
#define SVGA_REG_CONFIG_DONE          20
#define SVGA_REG_SYNC                 21
#define SVGA_REG_BUSY                 22
#define SVGA_REG_GUEST_ID             23
#define SVGA_REG_CURSOR_X             24
#define SVGA_REG_CURSOR_Y             25
#define SVGA_REG_CURSOR_ON            26
#define SVGA_REG_HOST_BITS_PER_PIXEL  27
#define SVGA_REG_GMR_ID               40
#define SVGA_REG_GMR_DESCRIPTOR       41
#define SVGA_REG_GMRS_MAX_PAGES       43
#define SVGA_REG_MEMORY_SIZE          44
#define SVGA_REG_MAX_GMR_IDS          45
#define SVGA_REG_MAX_GMR_DESCRIPTOR_LENGTH 46
#define SVGA_REG_MAX_GMR_PAGES        47

#define SVGA_CAP_GMR                  (1u << 5)
#define SVGA_CAP_GMR2                 (1u << 21)

#define SVGA_CMD_INVALID_CMD          0
#define SVGA_CMD_UPDATE               1
#define SVGA_CMD_RECT_FILL            2
#define SVGA_CMD_RECT_COPY            3
#define SVGA_CMD_DEFINE_GMRFB         18
#define SVGA_CMD_BLIT_GMRFB_TO_SCREEN 19
#define SVGA_CMD_BLIT_SCREEN_TO_GMRFB 20
#define SVGA_CMD_DEFINE_GMR2          24
#define SVGA_CMD_REMAP_GMR2           25

#define SVGA_FIFO_MIN                 0
#define SVGA_FIFO_MAX                 1
#define SVGA_FIFO_NEXT_CMD            2
#define SVGA_FIFO_STOP                3

#define SVGA_GMR_FRAMEBUFFER          1
#define SVGA_FIFO_SIZE                (256 * 1024)

static int svga3d_available = 0;
static pci_dev_t svga_dev;
static uint16_t svga_io_base = 0;
static uint32_t svga_fb_phys = 0;
static uint32_t svga_fb_virt = 0;
static uint32_t svga_fb_size = 0;
static uint32_t svga_fifo_phys = 0;
static volatile uint32_t* svga_fifo = 0;
static uint32_t svga_width = 0;
static uint32_t svga_height = 0;
static uint32_t svga_pitch = 0;
static uint32_t svga_bpp = 0;
static uint32_t svga_caps = 0;

static uint32_t gmr_fb_phys[2048];
static int gmr_fb_pages = 0;
static int gmr_defined = 0;

static inline uint32_t svga_read_reg(uint32_t reg) {
    outl(svga_io_base + SVGA_IO_INDEX, reg);
    return inl(svga_io_base + SVGA_IO_VALUE);
}

static inline void svga_write_reg(uint32_t reg, uint32_t val) {
    outl(svga_io_base + SVGA_IO_INDEX, reg);
    outl(svga_io_base + SVGA_IO_VALUE, val);
}

static inline void svga_fifo_write(uint32_t val) {
    uint32_t next = svga_fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t max = svga_fifo[SVGA_FIFO_MAX];
    uint32_t min = svga_fifo[SVGA_FIFO_MIN];
    volatile uint32_t* dst = (volatile uint32_t*)((uint8_t*)svga_fifo + next);
    *dst = val;
    next += 4;
    if (next >= max) next = min;
    svga_fifo[SVGA_FIFO_NEXT_CMD] = next;
}

static void svga_sync(void) {
    if (!svga3d_available || !svga_fifo) return;
    svga_write_reg(SVGA_REG_SYNC, 1);
    while (svga_read_reg(SVGA_REG_BUSY)) { }
}

static void svga_fifo_commit(void) {
    if (!svga3d_available || !svga_fifo) return;
    /* ring the doorbell */
    svga_write_reg(SVGA_REG_SYNC, 1);
}

int gpu_svga3d_probe(void) {
    if (pci_find_device(0x15AD, 0x0405, &svga_dev)) {
        serial_write("SVGA3D: VMware SVGA-II detected\n");
        return 1;
    }
    serial_write("SVGA3D: not available\n");
    return 0;
}

static void svga_setup_fifo(void) {
    svga_fifo_phys = svga_read_reg(SVGA_REG_MEM_START);
    uint32_t fifo_size = svga_read_reg(SVGA_REG_MEM_SIZE);
    if (fifo_size < 4 * PAGE_SIZE) fifo_size = 4 * PAGE_SIZE;
    if (fifo_size > SVGA_FIFO_SIZE) fifo_size = SVGA_FIFO_SIZE;

    uint32_t pages = (fifo_size + PAGE_SIZE - 1) / PAGE_SIZE;
    svga_fifo = (volatile uint32_t*)mmio_map(svga_fifo_phys, pages);
    if (!svga_fifo) {
        serial_write("SVGA3D: failed to map FIFO\n");
        svga3d_available = 0;
        return;
    }

    svga_fifo[SVGA_FIFO_MIN] = 16;
    svga_fifo[SVGA_FIFO_MAX] = fifo_size;
    svga_fifo[SVGA_FIFO_NEXT_CMD] = 16;
    svga_fifo[SVGA_FIFO_STOP] = 16;

    svga_write_reg(SVGA_REG_CONFIG_DONE, 1);
    serial_write("SVGA3D: FIFO configured\n");
}

static void svga_setup_gmr_framebuffer(void) {
    if (!(svga_caps & SVGA_CAP_GMR2)) {
        serial_write("SVGA3D: GMR2 not supported, HW blit unavailable\n");
        return;
    }

    int pages = gmr_fb_pages;
    if (pages <= 0 || pages > 2048) return;

    uint32_t max_gmr_ids = svga_read_reg(SVGA_REG_MAX_GMR_IDS);
    if (max_gmr_ids <= SVGA_GMR_FRAMEBUFFER) {
        serial_write("SVGA3D: not enough GMR IDs\n");
        return;
    }

    /* DEFINE_GMR2: gmrId, numPages */
    svga_fifo_write(SVGA_CMD_DEFINE_GMR2);
    svga_fifo_write(SVGA_GMR_FRAMEBUFFER);
    svga_fifo_write((uint32_t)pages);

    /* REMAP_GMR2: gmrId, flags, offsetPages, numPages, pfn... */
    svga_fifo_write(SVGA_CMD_REMAP_GMR2);
    svga_fifo_write(SVGA_GMR_FRAMEBUFFER);
    svga_fifo_write(0);                 /* flags */
    svga_fifo_write(0);                 /* offsetPages */
    svga_fifo_write((uint32_t)pages);
    for (int i = 0; i < pages; i++) {
        svga_fifo_write(gmr_fb_phys[i] >> 12);
    }

    svga_fifo_commit();
    svga_sync();

    /* DEFINE_GMRFB */
    uint32_t bpp = svga_bpp ? svga_bpp : 32;
    uint32_t format = (bpp & 0xFF) | (((bpp == 32) ? 24 : bpp) << 8);
    svga_fifo_write(SVGA_CMD_DEFINE_GMRFB);
    svga_fifo_write(format);
    svga_fifo_write(svga_pitch ? svga_pitch : svga_width * (bpp / 8));
    svga_fifo_write(0);                 /* ptr offset low */
    svga_fifo_write(SVGA_GMR_FRAMEBUFFER); /* ptr gmrId high */

    svga_fifo_commit();
    svga_sync();

    gmr_defined = 1;
    serial_write("SVGA3D: GMR framebuffer defined\n");
}

void gpu_svga3d_setup_gmr_fb(void) {
    svga_setup_gmr_framebuffer();
}

int gpu_svga3d_init(void) {
    if (svga3d_available) return 1;

    svga3d_available = 0;
    gmr_defined = 0;
    gmr_fb_pages = 0;

    if (!gpu_svga3d_probe()) {
        serial_write("SVGA3D: no device, fallback to software\n");
        return 0;
    }

    pci_enable_bus_master(&svga_dev);
    svga_io_base = (uint16_t)(svga_dev.bar[0] & 0xFFFC);
    if (svga_io_base == 0) {
        serial_write("SVGA3D: invalid IO base\n");
        return 0;
    }

    uint32_t id = svga_read_reg(SVGA_REG_ID);
    serial_write("SVGA3D: ID=");
    serial_hex(id);
    serial_write("\n");

    svga_write_reg(SVGA_REG_ID, 2); /* SVGA-II */
    id = svga_read_reg(SVGA_REG_ID);
    if (id != 2 && id != 0x90000002) {
        serial_write("SVGA3D: failed to set SVGA-II ID\n");
        return 0;
    }

    svga_write_reg(SVGA_REG_ENABLE, 0);

    svga_caps = svga_read_reg(SVGA_REG_CAPABILITIES);
    serial_write("SVGA3D: caps=");
    serial_hex(svga_caps);
    serial_write("\n");

    svga_fb_phys = svga_read_reg(SVGA_REG_FB_START);
    svga_fb_size = svga_read_reg(SVGA_REG_FB_SIZE);
    serial_write("SVGA3D: FB phys=");
    serial_hex(svga_fb_phys);
    serial_write(" size=");
    serial_hex(svga_fb_size);
    serial_write("\n");

    svga_setup_fifo();
    if (!svga_fifo) return 0;

    svga3d_available = 1;
    serial_write("SVGA3D: initialized\n");
    return 1;
}

int gpu_svga3d_set_mode(int w, int h, int bpp) {
    if (!svga3d_available) return 0;

    svga_write_reg(SVGA_REG_ENABLE, 0);
    svga_write_reg(SVGA_REG_WIDTH, (uint32_t)w);
    svga_write_reg(SVGA_REG_HEIGHT, (uint32_t)h);
    svga_write_reg(SVGA_REG_BITS_PER_PIXEL, (uint32_t)bpp);
    svga_write_reg(SVGA_REG_ENABLE, 1);
    svga_sync();

    svga_width = svga_read_reg(SVGA_REG_WIDTH);
    svga_height = svga_read_reg(SVGA_REG_HEIGHT);
    svga_bpp = svga_read_reg(SVGA_REG_BITS_PER_PIXEL);
    svga_pitch = svga_read_reg(SVGA_REG_BYTES_PER_LINE);
    if (svga_pitch == 0) svga_pitch = svga_width * (svga_bpp / 8);

    if (svga_fb_virt) {
        /* previous mapping leaked; not expected in current design */
    }

    uint32_t pages = (svga_fb_size + PAGE_SIZE - 1) / PAGE_SIZE;
    svga_fb_virt = mmio_map(svga_fb_phys, pages);
    if (!svga_fb_virt) {
        serial_write("SVGA3D: failed to map framebuffer\n");
        return 0;
    }

    serial_write("SVGA3D: mode ");
    serial_write("W="); serial_hex(svga_width);
    serial_write(" H="); serial_hex(svga_height);
    serial_write(" bpp="); serial_hex(svga_bpp);
    serial_write(" pitch="); serial_hex(svga_pitch);
    serial_write("\n");
    return 1;
}

void gpu_svga3d_register_fb_pages(const uint32_t* phys_pages, int count) {
    if (count > 2048) count = 2048;
    gmr_fb_pages = count;
    for (int i = 0; i < count; i++) gmr_fb_phys[i] = phys_pages[i];
}

int gpu_svga3d_is_available(void) {
    return svga3d_available;
}

int gpu_svga3d_is_gmr_ready(void) {
    return gmr_defined;
}

uint32_t gpu_svga3d_get_fb(void) {
    return svga_fb_virt;
}

uint32_t gpu_svga3d_get_width(void) {
    return svga_width;
}

uint32_t gpu_svga3d_get_height(void) {
    return svga_height;
}

uint32_t gpu_svga3d_get_pitch(void) {
    return svga_pitch;
}

void gpu_svga3d_update_rect(int x, int y, int w, int h) {
    if (!svga3d_available || !svga_fifo) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)svga_width) w = (int)svga_width - x;
    if (y + h > (int)svga_height) h = (int)svga_height - y;
    if (w <= 0 || h <= 0) return;

    svga_fifo_write(SVGA_CMD_UPDATE);
    svga_fifo_write((uint32_t)x);
    svga_fifo_write((uint32_t)y);
    svga_fifo_write((uint32_t)w);
    svga_fifo_write((uint32_t)h);
    svga_fifo_commit();
}

void gpu_svga3d_fillrect(int x, int y, int w, int h, uint32_t color) {
    if (!svga3d_available || !svga_fifo) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)svga_width) w = (int)svga_width - x;
    if (y + h > (int)svga_height) h = (int)svga_height - y;
    if (w <= 0 || h <= 0) return;

    svga_fifo_write(SVGA_CMD_RECT_FILL);
    svga_fifo_write(color);
    svga_fifo_write((uint32_t)x);
    svga_fifo_write((uint32_t)y);
    svga_fifo_write((uint32_t)w);
    svga_fifo_write((uint32_t)h);
    svga_fifo_commit();
}

void gpu_svga3d_blit_gmr_to_screen(int sx, int sy, int dx, int dy, int w, int h) {
    if (!svga3d_available || !svga_fifo || !gmr_defined) return;
    if (w <= 0 || h <= 0) return;

    svga_fifo_write(SVGA_CMD_BLIT_GMRFB_TO_SCREEN);
    svga_fifo_write((uint32_t)sx);   /* srcOriginX */
    svga_fifo_write((uint32_t)sy);   /* srcOriginY */
    svga_fifo_write((uint32_t)dx);   /* destX */
    svga_fifo_write((uint32_t)dy);   /* destY */
    svga_fifo_write((uint32_t)w);    /* destWidth */
    svga_fifo_write((uint32_t)h);    /* destHeight */
    svga_fifo_commit();
}

void gpu_svga3d_blit_all(int w, int h) {
    gpu_svga3d_blit_gmr_to_screen(0, 0, 0, 0, w, h);
}

void gpu_svga3d_sync(void) {
    svga_sync();
}

void gpu_svga3d_draw_triangle(fixed_t* v0, fixed_t* v1, fixed_t* v2, uint32_t color) {
    (void)v0; (void)v1; (void)v2; (void)color;
    /* 3D triangle rasterization not yet accelerated */
}