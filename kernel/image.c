#include "gui_internal.h"
#include "image.h"
#include "bmp.h"
#include "jpeg.h"
#include "serial.h"
#include <stdint.h>

static inline uint32_t image_blend(uint32_t dst, uint32_t src) {
    uint32_t a = src >> 24;
    if (a == 0) return dst;
    if (a == 255) return src & 0x00FFFFFF;
    uint32_t inv = 255 - a;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >> 8) & 0xFF;
    uint32_t sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >> 8) & 0xFF;
    uint32_t db = dst & 0xFF;
    uint32_t r = (sr * a + dr * inv) / 255;
    uint32_t g = (sg * a + dg * inv) / 255;
    uint32_t b = (sb * a + db * inv) / 255;
    return (r << 16) | (g << 8) | b;
}

void image_blit(int x, int y, const uint32_t* data, int w, int h) {
    if (!back_buffer || !data || w <= 0 || h <= 0) return;

    int sx = 0, sy = 0, ex = w, ey = h;
    if (x < 0) sx = -x;
    if (y < 0) sy = -y;
    if (x + ex > (int)fb_width) ex = (int)fb_width - x;
    if (y + ey > (int)fb_height) ey = (int)fb_height - y;
    if (sx >= ex || sy >= ey) return;

    if (clip_enabled) {
        if (x + sx < clip_x1) sx = clip_x1 - x;
        if (x + ex > clip_x2) ex = clip_x2 - x;
        if (y + sy < clip_y1) sy = clip_y1 - y;
        if (y + ey > clip_y2) ey = clip_y2 - y;
        if (sx >= ex || sy >= ey) return;
    }

    int pitch = (int)(fb_pitch / 4);
    for (int row = sy; row < ey; row++) {
        const uint32_t* src = data + row * w + sx;
        uint32_t* dst = back_buffer + (y + row) * pitch + (x + sx);
        for (int col = sx; col < ex; col++) {
            uint32_t p = *src++;
            uint32_t a = p >> 24;
            if (a == 255) {
                *dst = p & 0x00FFFFFF;
            } else if (a != 0) {
                *dst = image_blend(*dst, p);
            }
            dst++;
        }
    }
}

static int img_streq_ci(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        i++;
    }
    return a[i] == 0 && b[i] == 0;
}

static const char* img_ext(const char* path) {
    int last_dot = -1;
    int i = 0;
    while (path[i]) {
        if (path[i] == '.') last_dot = i;
        i++;
    }
    if (last_dot < 0) return "";
    return path + last_dot;
}

int image_load(const char* path, uint32_t** out_pixels, int* out_w, int* out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;
    *out_pixels = 0;
    *out_w = 0;
    *out_h = 0;
    const char* ext = img_ext(path);
    if (img_streq_ci(ext, ".bmp")) {
        return bmp_decode(path, out_pixels, out_w, out_h);
    }
    if (img_streq_ci(ext, ".jpg") || img_streq_ci(ext, ".jpeg")) {
        return jpeg_decode(path, out_pixels, out_w, out_h);
    }
    serial_write("image_load: unknown extension ");
    serial_write(ext);
    serial_write("\n");
    return -1;
}
