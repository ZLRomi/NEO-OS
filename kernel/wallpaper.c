#include "wallpaper.h"
#include "gui_internal.h"
#include "image.h"
#include "fat32.h"
#include "heap.h"
#include "serial.h"
#include "klog.h"
#include "generated/asset_wallpaper.h"
#include <stdint.h>
#include <stddef.h>

#define WALLPAPER_CONF_PATH  "/etc/wallpaper.conf"
#define WALLPAPER_CONF_SIZE  512

#define WALLPAPER_GRAD_TOP    0x004A90E2
#define WALLPAPER_GRAD_BOTTOM 0x001E3A8A

int g_wallpaper_loaded = 0;
int g_wallpaper_mode = WALLPAPER_STRETCH;
char g_wallpaper_path[WALLPAPER_PATH_MAX] = {0};

static uint32_t* s_raw_pixels = NULL;
static int s_raw_w = 0;
static int s_raw_h = 0;
static uint32_t* s_scaled_buf = NULL;
static int s_scaled_w = 0;
static int s_scaled_h = 0;
static int s_scale_failed = 0;

static int wp_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void wp_memcpy(char* dst, const char* src, int n) {
    int i;
    for (i = 0; i < n; i++) dst[i] = src[i];
}

static int wp_starts_with(const char* s, const char* prefix) {
    int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static void parse_line(const char* line) {
    if (line[0] == '#' || line[0] == 0) return;
    if (wp_starts_with(line, "path=")) {
        const char* v = line + 5;
        int len = wp_strlen(v);
        if (len >= WALLPAPER_PATH_MAX) len = WALLPAPER_PATH_MAX - 1;
        wp_memcpy(g_wallpaper_path, v, len);
        g_wallpaper_path[len] = 0;
    } else if (wp_starts_with(line, "mode=")) {
        const char* v = line + 5;
        if (v[0] == '0' || (v[0] == 's' && v[1] == 't')) g_wallpaper_mode = WALLPAPER_STRETCH;
        else if (v[0] == '1' || v[0] == 'c') g_wallpaper_mode = WALLPAPER_CENTER;
        else if (v[0] == '2' || (v[0] == 's' && v[1] == 'c')) g_wallpaper_mode = WALLPAPER_SCALE;
    }
}

static void parse_config(const char* buf, int total) {
    int start = 0;
    int i = 0;
    while (i <= total) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            char line[300];
            int o = 0;
            int s = start;
            while (s < i && buf[s] == '\r') s++;
            while (s < i && buf[s] != '\r' && o < 299) {
                line[o++] = buf[s++];
            }
            line[o] = 0;
            parse_line(line);
            if (buf[i] == '\0') break;
            start = i + 1;
        }
        i++;
    }
}

static void free_scaled(void) {
    if (s_scaled_buf) {
        kfree(s_scaled_buf);
        s_scaled_buf = NULL;
    }
    s_scaled_w = 0;
    s_scaled_h = 0;
    s_scale_failed = 0;
}

static void free_raw(void) {
    if (s_raw_pixels) {
        kfree(s_raw_pixels);
        s_raw_pixels = NULL;
    }
    s_raw_w = 0;
    s_raw_h = 0;
}

static void fill_gradient_buffer(uint32_t* buf, int w, int h) {
    int r1 = (WALLPAPER_GRAD_TOP >> 16) & 0xFF;
    int g1 = (WALLPAPER_GRAD_TOP >> 8) & 0xFF;
    int b1 = WALLPAPER_GRAD_TOP & 0xFF;
    int r2 = (WALLPAPER_GRAD_BOTTOM >> 16) & 0xFF;
    int g2 = (WALLPAPER_GRAD_BOTTOM >> 8) & 0xFF;
    int b2 = WALLPAPER_GRAD_BOTTOM & 0xFF;
    int y;
    for (y = 0; y < h; y++) {
        int t = (h > 0) ? (y * 255 / h) : 0;
        int r = r1 + (r2 - r1) * t / 255;
        int g = g1 + (g2 - g1) * t / 255;
        int b = b1 + (b2 - b1) * t / 255;
        uint32_t c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        uint32_t* row = buf + y * w;
        int x;
        for (x = 0; x < w; x++) row[x] = c;
    }
}

static void build_scaled(void) {
    serial_write("BSC: enter\n");
    free_scaled();
    serial_write("BSC: free_scaled done\n");
    int sw = (int)fb_width;
    int sh = (int)fb_height;
    if (sw <= 0 || sh <= 0) { serial_write("BSC: bad size\n"); return; }
    if (!s_raw_pixels || s_raw_w <= 0 || s_raw_h <= 0) {
        serial_write("BSC: no raw pixels\n");
        return;
    }
    serial_write("BSC: calling kmalloc\n");
    uint32_t* buf = (uint32_t*)kmalloc((size_t)sw * (size_t)sh * 4);
    if (!buf) {
        s_scale_failed = 1;
        serial_write("wallpaper: oom for scaled buffer\n");
        klog_write_level(KLOG_ERROR, "wallpaper: out of memory for scaled buffer (%dx%d)\n", sw, sh);
        return;
    }
    serial_write("BSC: kmalloc done\n");
    serial_write("BSC: s_raw_pixels=");
    { uint32_t rp = (uint32_t)(uintptr_t)s_raw_pixels; serial_write_hex((const uint8_t*)&rp, 4); }
    serial_write(" buf=");
    { uint32_t bp = (uint32_t)(uintptr_t)buf; serial_write_hex((const uint8_t*)&bp, 4); }
    serial_write("\n");

    if (g_wallpaper_mode == WALLPAPER_STRETCH) {
        serial_write("BSC: stretch loop start\n");
        int dy;
        for (dy = 0; dy < sh; dy++) {
            if (dy == 0) { serial_write("BSC: stretch dy=0\n"); }
            int sy = dy * s_raw_h / sh;
            if (sy < 0) sy = 0;
            if (sy >= s_raw_h) sy = s_raw_h - 1;
            const uint32_t* src_row = s_raw_pixels + sy * s_raw_w;
            uint32_t* dst_row = buf + dy * sw;
            int dx;
            for (dx = 0; dx < sw; dx++) {
                int sx = dx * s_raw_w / sw;
                if (sx < 0) sx = 0;
                if (sx >= s_raw_w) sx = s_raw_w - 1;
                dst_row[dx] = src_row[sx] | 0xFF000000u;
            }
        }
        serial_write("BSC: stretch loop done\n");
    } else {
        fill_gradient_buffer(buf, sw, sh);
        if (g_wallpaper_mode == WALLPAPER_CENTER) {
            int ox = (sw - s_raw_w) / 2;
            int oy = (sh - s_raw_h) / 2;
            int y;
            for (y = 0; y < s_raw_h; y++) {
                int dy = oy + y;
                if (dy < 0 || dy >= sh) continue;
                const uint32_t* src_row = s_raw_pixels + y * s_raw_w;
                uint32_t* dst_row = buf + dy * sw;
                int x;
                for (x = 0; x < s_raw_w; x++) {
                    int dx = ox + x;
                    if (dx < 0 || dx >= sw) continue;
                    dst_row[dx] = src_row[x] | 0xFF000000u;
                }
            }
        } else {
            int scaled_w = sw;
            int scaled_h = s_raw_h * sw / (s_raw_w > 0 ? s_raw_w : 1);
            if (scaled_h > sh) {
                scaled_h = sh;
                scaled_w = s_raw_w * sh / (s_raw_h > 0 ? s_raw_h : 1);
            }
            if (scaled_w > 0 && scaled_h > 0) {
                int ox = (sw - scaled_w) / 2;
                int oy = (sh - scaled_h) / 2;
                int dy;
                for (dy = 0; dy < scaled_h; dy++) {
                    int sy = dy * s_raw_h / scaled_h;
                    if (sy < 0) sy = 0;
                    if (sy >= s_raw_h) sy = s_raw_h - 1;
                    const uint32_t* src_row = s_raw_pixels + sy * s_raw_w;
                    uint32_t* dst_row = buf + (oy + dy) * sw;
                    int dx;
                    for (dx = 0; dx < scaled_w; dx++) {
                        int sx = dx * s_raw_w / scaled_w;
                        if (sx < 0) sx = 0;
                        if (sx >= s_raw_w) sx = s_raw_w - 1;
                        if (ox + dx >= 0 && ox + dx < sw) {
                            dst_row[ox + dx] = src_row[sx] | 0xFF000000u;
                        }
                    }
                }
            }
        }
    }

    s_scaled_buf = buf;
    s_scaled_w = sw;
    s_scaled_h = sh;
    serial_write("wallpaper: scaled buffer built\n");
}

int wallpaper_load_embedded_default(void) {
    free_raw();
    free_scaled();
    g_wallpaper_loaded = 0;

    int w = ASSET_WALLPAPER_W;
    int h = ASSET_WALLPAPER_H;
    size_t size = (size_t)w * (size_t)h * 4;
    uint32_t* px = (uint32_t*)kmalloc(size);
    if (!px) {
        serial_write("wallpaper: oom for embedded wallpaper\n");
        klog_write_level(KLOG_ERROR, "wallpaper: out of memory for embedded wallpaper\n");
        return -1;
    }
    const uint32_t* src = (const uint32_t*)asset_wallpaper;
    for (int i = 0; i < w * h; i++) px[i] = src[i];

    s_raw_pixels = px;
    s_raw_w = w;
    s_raw_h = h;
    g_wallpaper_loaded = 1;
    g_wallpaper_mode = WALLPAPER_SCALE;
    g_wallpaper_path[0] = 0;

    if ((int)fb_width > 0 && (int)fb_height > 0) {
        build_scaled();
    }
    serial_write("wallpaper: loaded embedded default\n");
    return 0;
}

int wallpaper_load(const char* path, int mode) {
    free_raw();
    free_scaled();
    g_wallpaper_loaded = 0;
    if (!path || !path[0]) return -1;

    int len = wp_strlen(path);
    if (len >= WALLPAPER_PATH_MAX) len = WALLPAPER_PATH_MAX - 1;
    wp_memcpy(g_wallpaper_path, path, len);
    g_wallpaper_path[len] = 0;
    g_wallpaper_mode = mode;

    uint32_t* px = NULL;
    int w = 0;
    int h = 0;
    if (image_load(path, &px, &w, &h) != 0 || !px || w <= 0 || h <= 0) {
        serial_write("wallpaper: load failed: ");
        serial_write(path);
        serial_write("\n");
        if (px) kfree(px);
        return -1;
    }
    s_raw_pixels = px;
    s_raw_w = w;
    s_raw_h = h;
    g_wallpaper_loaded = 1;

    if ((int)fb_width > 0 && (int)fb_height > 0) {
        build_scaled();
    }
    serial_write("wallpaper: loaded ");
    serial_write(path);
    serial_write("\n");
    return 0;
}

void wallpaper_init(void) {
    g_wallpaper_loaded = 0;
    g_wallpaper_mode = WALLPAPER_STRETCH;
    g_wallpaper_path[0] = 0;

    int rc = -1;

    if (!fat32_is_mounted()) {
        rc = wallpaper_load_embedded_default();
        if (rc < 0) {
            klog_write_level(KLOG_ERROR, "wallpaper: failed to load embedded default\n");
        }
        return;
    }

    int fd = fat32_open(WALLPAPER_CONF_PATH, 0);
    if (fd < 0) {
        rc = wallpaper_load_embedded_default();
        if (rc < 0) {
            klog_write_level(KLOG_ERROR, "wallpaper: failed to load embedded default\n");
        }
        return;
    }

    static char buf[WALLPAPER_CONF_SIZE];
    int total = 0;
    int n;
    while (total < WALLPAPER_CONF_SIZE - 1 &&
           (n = fat32_read(fd, buf + total, WALLPAPER_CONF_SIZE - 1 - total)) > 0) {
        total += n;
    }
    fat32_close(fd);
    buf[total] = 0;

    parse_config(buf, total);
    if (g_wallpaper_path[0]) {
        rc = wallpaper_load(g_wallpaper_path, g_wallpaper_mode);
        if (rc < 0) {
            rc = wallpaper_load_embedded_default();
        }
    } else {
        rc = wallpaper_load_embedded_default();
    }

    if (rc < 0) {
        klog_write_level(KLOG_ERROR, "wallpaper: failed to load any wallpaper\n");
    }
}

void wallpaper_render(void) {
    int sw = (int)fb_width;
    int sh = (int)fb_height;
    serial_write("WPR: SKIP wallpaper, gradient only\n");
    if (sw <= 0 || sh <= 0) return;
    fb_fillrect_grad(0, 0, sw, sh, WALLPAPER_GRAD_TOP, WALLPAPER_GRAD_BOTTOM);
    serial_write("WPR: gradient done\n");
}

void wallpaper_save_config(void) {
    if (!fat32_is_mounted()) return;

    int dh = fat32_opendir("/etc");
    if (dh >= 0) {
        fat32_close(dh);
    } else {
        fat32_mkdir("/etc");
    }

    fat32_remove(WALLPAPER_CONF_PATH);
    int fd = fat32_open(WALLPAPER_CONF_PATH, 1);
    if (fd < 0) {
        serial_write("wallpaper: cannot create config\n");
        return;
    }

    static char buf[512];
    char* p = buf;
    const char* prefix = "path=";
    while (*prefix) *p++ = *prefix++;
    const char* s = g_wallpaper_path;
    while (*s) *p++ = *s++;
    *p++ = '\n';

    const char* prefix2 = "mode=";
    while (*prefix2) *p++ = *prefix2++;
    const char* m = (g_wallpaper_mode == WALLPAPER_STRETCH) ? "stretch" :
                    (g_wallpaper_mode == WALLPAPER_CENTER) ? "center" : "scale";
    while (*m) *p++ = *m++;
    *p++ = '\n';

    int len = (int)(p - buf);
    int written = fat32_write(fd, buf, len);
    fat32_close(fd);
    if (written != len) {
        serial_write("wallpaper: short write\n");
    } else {
        serial_write("wallpaper: saved config\n");
    }
}