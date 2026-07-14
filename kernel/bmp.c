#include "bmp.h"
#include "fat32.h"
#include "heap.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

static uint8_t* read_file_all(const char* path, uint32_t* out_size) {
    int fd = fat32_open(path, 0);
    if (fd < 0) return 0;
    uint32_t total = 0;
    {
        uint8_t tmp[1024];
        int n;
        while ((n = fat32_read(fd, tmp, sizeof(tmp))) > 0) total += (uint32_t)n;
    }
    fat32_close(fd);
    if (total == 0) return 0;

    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return 0;

    fd = fat32_open(path, 0);
    if (fd < 0) { kfree(buf); return 0; }
    uint32_t got = 0;
    while (got < total) {
        int n = fat32_read(fd, buf + got, (int)(total - got));
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    fat32_close(fd);
    if (got != total) { kfree(buf); return 0; }
    *out_size = total;
    return buf;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int bmp_decode(const char* path, uint32_t** out_pixels, int* out_w, int* out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;
    *out_pixels = 0;
    *out_w = 0;
    *out_h = 0;

    uint32_t fsize = 0;
    uint8_t* data = read_file_all(path, &fsize);
    if (!data) {
        serial_write("bmp: cannot read ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    if (fsize < 54 || data[0] != 'B' || data[1] != 'M') {
        serial_write("bmp: bad signature\n");
        kfree(data);
        return -1;
    }

    uint32_t pix_off = rd32(data + 10);
    uint32_t dib_size = rd32(data + 14);
    if (dib_size < 40 || dib_size > 124) {
        serial_write("bmp: unsupported dib\n");
        kfree(data);
        return -1;
    }
    if (14 + dib_size > fsize) {
        serial_write("bmp: truncated header\n");
        kfree(data);
        return -1;
    }

    int32_t width = (int32_t)rd32(data + 18);
    int32_t height_raw = (int32_t)rd32(data + 22);
    uint16_t bpp = rd16(data + 28);
    uint32_t compression = rd32(data + 30);

    if (width <= 0) {
        serial_write("bmp: bad width\n");
        kfree(data);
        return -1;
    }
    int top_down = 0;
    int32_t height;
    if (height_raw < 0) {
        top_down = 1;
        height = -height_raw;
    } else {
        height = height_raw;
    }
    if (height == 0) {
        serial_write("bmp: bad height\n");
        kfree(data);
        return -1;
    }
    if (compression != 0) {
        serial_write("bmp: compressed unsupported\n");
        kfree(data);
        return -1;
    }
    if (bpp != 24 && bpp != 32) {
        serial_write("bmp: only 24/32bpp\n");
        kfree(data);
        return -1;
    }
    if (pix_off > fsize) {
        serial_write("bmp: bad pixel offset\n");
        kfree(data);
        return -1;
    }

    int bytes_per_px = bpp / 8;
    int row_bytes = width * bytes_per_px;
    int row_padded = (row_bytes + 3) & ~3;
    if ((uint64_t)pix_off + (uint64_t)row_padded * (uint64_t)height > (uint64_t)fsize) {
        serial_write("bmp: truncated pixel data\n");
        kfree(data);
        return -1;
    }

    uint32_t* out = (uint32_t*)kmalloc((size_t)width * height * 4);
    if (!out) {
        serial_write("bmp: oom\n");
        kfree(data);
        return -1;
    }

    for (int y = 0; y < height; y++) {
        const uint8_t* row = data + pix_off + (top_down ? y : (height - 1 - y)) * row_padded;
        uint32_t* dst = out + (size_t)y * width;
        for (int x = 0; x < width; x++) {
            const uint8_t* px = row + x * bytes_per_px;
            uint8_t b = px[0];
            uint8_t g = px[1];
            uint8_t r = px[2];
            uint8_t a = (bpp == 32) ? px[3] : 255;
            dst[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | (uint32_t)b;
        }
    }

    kfree(data);
    *out_pixels = out;
    *out_w = width;
    *out_h = height;
    return 0;
}
