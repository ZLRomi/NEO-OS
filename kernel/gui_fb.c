#include "gui_internal.h"
#include "memory.h"
#include "gfx_driver.h"
#include "gpu_svga3d.h"
#include <stdint.h>

uint32_t* back_buffer = 0;
uint32_t fb_width = 0;
uint32_t fb_height = 0;
uint32_t fb_pitch = 0;

int clip_enabled = 0;
int clip_x1 = 0, clip_y1 = 0, clip_x2 = 0, clip_y2 = 0;

dirty_rect_t dirty_rects[DIRTY_RECT_MAX];
int dirty_count = 0;

#define FB_PAGE_SIZE 4096
#define FB_MAX_PAGES 4096
static uint32_t fb_phys_pages[FB_MAX_PAGES];
static int fb_page_count = 0;

void fb_set_clip(int x, int y, int w, int h) {
    clip_x1 = x;
    clip_y1 = y;
    clip_x2 = x + w;
    clip_y2 = y + h;
    clip_enabled = 1;
}

void fb_reset_clip(void) {
    clip_enabled = 0;
}

static inline int fb_clip_contains(int x, int y) {
    if (!clip_enabled) return 1;
    return (x >= clip_x1 && x < clip_x2 && y >= clip_y1 && y < clip_y2);
}

int fb_init(void) {
    fb_width = (uint32_t)gfx_get_width();
    fb_height = (uint32_t)gfx_get_height();
    fb_pitch = (uint32_t)gfx_get_pitch();
    if (fb_width == 0 || fb_height == 0 || fb_pitch == 0) {
        /* fallback to VBE if gfx driver has not set mode yet */
        fb_width = vbe_get_width();
        fb_height = vbe_get_height();
        fb_pitch = vbe_get_pitch();
    }
    uint32_t size = fb_height * fb_pitch;
    uint32_t pages = (size + FB_PAGE_SIZE - 1) / FB_PAGE_SIZE;
    if (pages > FB_MAX_PAGES) pages = FB_MAX_PAGES;

    static uint32_t next_virt = 0xF0000000;
    uint32_t virt = next_virt;
    fb_page_count = 0;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) return 0;
        if (!map_page(virt + i * FB_PAGE_SIZE, phys, PAGE_PRESENT | PAGE_WRITE)) {
            pmm_free_page(phys);
            return 0;
        }
        fb_phys_pages[fb_page_count++] = phys;
    }
    next_virt += pages * FB_PAGE_SIZE;

    back_buffer = (uint32_t*)virt;
    uint32_t words = size / 4;
    for (uint32_t i = 0; i < words; i++) back_buffer[i] = 0;

    if (gpu_svga3d_is_available() && fb_page_count > 0) {
        gpu_svga3d_register_fb_pages(fb_phys_pages, fb_page_count);
        gpu_svga3d_setup_gmr_fb();
    }
    return 1;
}

void fb_free(void) {
    back_buffer = 0;
}

void fb_blit(void) {
    if (!back_buffer) return;

    if (gpu_svga3d_is_available()) {
        if (gpu_svga3d_is_gmr_ready()) {
            gpu_svga3d_blit_all((int)fb_width, (int)fb_height);
            gpu_svga3d_sync();
            return;
        }
        uint32_t* front = (uint32_t*)gfx_get_fb();
        if (front) {
            int pitch_words = (int)(fb_pitch / 4);
            for (uint32_t y = 0; y < fb_height; y++) {
                for (int x = 0; x < pitch_words; x++) {
                    front[y * pitch_words + x] = back_buffer[y * pitch_words + x];
                }
            }
            gpu_svga3d_update_rect(0, 0, (int)fb_width, (int)fb_height);
        }
        gpu_svga3d_sync();
        return;
    }

    uint32_t* front = (uint32_t*)vbe_get_fb();
    if (!front) return;
    int pitch_words = (int)(fb_pitch / 4);
    for (uint32_t y = 0; y < fb_height; y++) {
        for (int x = 0; x < pitch_words; x++) {
            front[y * pitch_words + x] = back_buffer[y * pitch_words + x];
        }
    }
}

void dirty_clear(void) {
    dirty_count = 0;
}

void mark_dirty(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_width) w = (int)fb_width - x;
    if (y + h > (int)fb_height) h = (int)fb_height - y;
    if (w <= 0 || h <= 0) return;
    if (dirty_count >= DIRTY_RECT_MAX) {
        for (int i = 1; i < dirty_count; i++) {
            int x1 = dirty_rects[0].x < dirty_rects[i].x ? dirty_rects[0].x : dirty_rects[i].x;
            int y1 = dirty_rects[0].y < dirty_rects[i].y ? dirty_rects[0].y : dirty_rects[i].y;
            int ex0 = dirty_rects[0].x + dirty_rects[0].w;
            int ey0 = dirty_rects[0].y + dirty_rects[0].h;
            int ex1 = dirty_rects[i].x + dirty_rects[i].w;
            int ey1 = dirty_rects[i].y + dirty_rects[i].h;
            int x2 = ex0 > ex1 ? ex0 : ex1;
            int y2 = ey0 > ey1 ? ey0 : ey1;
            dirty_rects[0].x = x1;
            dirty_rects[0].y = y1;
            dirty_rects[0].w = x2 - x1;
            dirty_rects[0].h = y2 - y1;
        }
        dirty_count = 1;
    }
    dirty_rects[dirty_count].x = x;
    dirty_rects[dirty_count].y = y;
    dirty_rects[dirty_count].w = w;
    dirty_rects[dirty_count].h = h;
    dirty_count++;
}

void fb_blit_dirty(void) {
    if (!back_buffer) return;
    if (dirty_count <= 0) return;

    if (gpu_svga3d_is_available()) {
        if (gpu_svga3d_is_gmr_ready()) {
            for (int i = 0; i < dirty_count; i++) {
                int x = dirty_rects[i].x;
                int y = dirty_rects[i].y;
                int w = dirty_rects[i].w;
                int h = dirty_rects[i].h;
                gpu_svga3d_blit_gmr_to_screen(x, y, x, y, w, h);
            }
            gpu_svga3d_sync();
            return;
        }
        uint32_t* front = (uint32_t*)gfx_get_fb();
        if (front) {
            int pitch_words = (int)(fb_pitch / 4);
            for (int i = 0; i < dirty_count; i++) {
                int x1 = dirty_rects[i].x;
                int y1 = dirty_rects[i].y;
                int x2 = x1 + dirty_rects[i].w;
                int y2 = y1 + dirty_rects[i].h;
                for (int y = y1; y < y2; y++) {
                    uint32_t* src = back_buffer + y * pitch_words + x1;
                    uint32_t* dst = front + y * pitch_words + x1;
                    for (int x = x1; x < x2; x++) {
                        *dst++ = *src++;
                    }
                }
            }
            gpu_svga3d_update_rect(0, 0, (int)fb_width, (int)fb_height);
        }
        gpu_svga3d_sync();
        return;
    }

    uint32_t* front = (uint32_t*)vbe_get_fb();
    if (!front) return;
    int pitch_words = (int)(fb_pitch / 4);
    for (int i = 0; i < dirty_count; i++) {
        int x1 = dirty_rects[i].x;
        int y1 = dirty_rects[i].y;
        int x2 = x1 + dirty_rects[i].w;
        int y2 = y1 + dirty_rects[i].h;
        for (int y = y1; y < y2; y++) {
            uint32_t* src = back_buffer + y * pitch_words + x1;
            uint32_t* dst = front + y * pitch_words + x1;
            for (int x = x1; x < x2; x++) {
                *dst++ = *src++;
            }
        }
    }
}

void cursor_erase_front(int x, int y, int w, int h) {
    uint32_t* front = (uint32_t*)gfx_get_fb();
    if (!front || !back_buffer) return;
    int pitch = (int)(fb_pitch / 4);
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > (int)fb_width) x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;
    for (int row = y1; row < y2; row++) {
        uint32_t* dst = front + row * pitch + x1;
        uint32_t* src = back_buffer + row * pitch + x1;
        for (int col = x1; col < x2; col++) {
            *dst++ = *src++;
        }
    }
}

void fb_putpixel(int x, int y, uint32_t color) {
    if (!back_buffer) return;
    if (x < 0 || y < 0 || (uint32_t)x >= fb_width || (uint32_t)y >= fb_height) return;
    if (!fb_clip_contains(x, y)) return;
    back_buffer[y * (fb_pitch / 4) + x] = color;
}

void fb_fillrect_grad(int x, int y, int w, int h, uint32_t top, uint32_t bottom) {
    for (int row = 0; row < h; row++) {
        int r1 = (top >> 16) & 0xFF, g1 = (top >> 8) & 0xFF, b1 = top & 0xFF;
        int r2 = (bottom >> 16) & 0xFF, g2 = (bottom >> 8) & 0xFF, b2 = bottom & 0xFF;
        int t = row * 255 / h;
        int r = r1 + (r2 - r1) * t / 255;
        int g = g1 + (g2 - g1) * t / 255;
        int b = b1 + (b2 - b1) * t / 255;
        uint32_t color = 0x00000000 | (r << 16) | (g << 8) | b;
        fb_fillrect(x, y + row, w, 1, color);
    }
}

void fb_fillrect_vgrad(int x, int y, int w, int h, uint32_t color_top, uint32_t color_bottom) {
    if (h <= 0 || w <= 0) return;
    int r1 = (color_top >> 16) & 0xFF;
    int g1 = (color_top >> 8) & 0xFF;
    int b1 = color_top & 0xFF;
    int r2 = (color_bottom >> 16) & 0xFF;
    int g2 = (color_bottom >> 8) & 0xFF;
    int b2 = color_bottom & 0xFF;
    int denom = h > 1 ? h - 1 : 1;
    for (int row = 0; row < h; row++) {
        int r = r1 + (r2 - r1) * row / denom;
        int g = g1 + (g2 - g1) * row / denom;
        int b = b1 + (b2 - b1) * row / denom;
        uint32_t color = 0x00000000 | (r << 16) | (g << 8) | b;
        fb_fillrect(x, y + row, w, 1, color);
    }
}

void fb_fillrect_alpha(int x, int y, int w, int h, uint32_t color) {
    if (!back_buffer || w <= 0 || h <= 0) return;
    int alpha = (color >> 24) & 0xFF;
    if (alpha == 0) return;
    int cr = (color >> 16) & 0xFF;
    int cg = (color >> 8) & 0xFF;
    int cb = color & 0xFF;
    int pitch_words = (int)(fb_pitch / 4);
    int x1 = x < 0 ? 0 : x;
    int y1 = y < 0 ? 0 : y;
    int x2 = x + w;
    int y2 = y + h;
    if (x2 > (int)fb_width) x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;
    if (clip_enabled) {
        if (x1 < clip_x1) x1 = clip_x1;
        if (y1 < clip_y1) y1 = clip_y1;
        if (x2 > clip_x2) x2 = clip_x2;
        if (y2 > clip_y2) y2 = clip_y2;
    }
    if (x1 >= x2 || y1 >= y2) return;
    int inv = 255 - alpha;
    for (int row = y1; row < y2; row++) {
        uint32_t* dst = back_buffer + row * pitch_words + x1;
        for (int col = x1; col < x2; col++) {
            uint32_t bg = *dst;
            int br = (bg >> 16) & 0xFF;
            int bgg = (bg >> 8) & 0xFF;
            int bb = bg & 0xFF;
            int nr = (cr * alpha + br * inv) / 255;
            int ng = (cg * alpha + bgg * inv) / 255;
            int nb = (cb * alpha + bb * inv) / 255;
            *dst++ = 0x00000000 | (nr << 16) | (ng << 8) | nb;
        }
    }
}

void fb_draw_rect_outline(int x, int y, int w, int h, uint32_t color) {
    fb_fillrect(x, y, w, 1, color);
    fb_fillrect(x, y + h - 1, w, 1, color);
    fb_fillrect(x, y, 1, h, color);
    fb_fillrect(x + w - 1, y, 1, h, color);
}

void fb_draw_line(int x1, int y1, int x2, int y2, uint32_t color) {
    int dx = x2 - x1;
    int dy = y2 - y1;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    int x = x1, y = y1;
    while (1) {
        fb_putpixel(x, y, color);
        if (x == x2 && y == y2) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

void fb_draw_circle(int cx, int cy, int r, uint32_t color) {
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                fb_putpixel(cx + x, cy + y, color);
            }
        }
    }
}

void fb_draw_round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    fb_fillrect(x + r, y, w - 2 * r, h, color);
    fb_fillrect(x, y + r, w, h - 2 * r, color);
    fb_fill_circle(x + r, y + r, r, color);
    fb_fill_circle(x + w - r - 1, y + r, r, color);
    fb_fill_circle(x + r, y + h - r - 1, r, color);
    fb_fill_circle(x + w - r - 1, y + h - r - 1, r, color);
}

void fb_draw_round_rect_top(int x, int y, int w, int h, int r, uint32_t color) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h) r = h;
    fb_fillrect(x + r, y, w - 2 * r, h, color);
    fb_fillrect(x, y + r, w, h - r, color);
    fb_fill_circle(x + r, y + r, r, color);
    fb_fill_circle(x + w - r - 1, y + r, r, color);
}

void fb_fillrect_vgrad_round_top(int x, int y, int w, int h, int r,
                                  uint32_t color_top, uint32_t color_bottom) {
    if (h <= 0 || w <= 0) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h) r = h;
    int r1 = (color_top >> 16) & 0xFF;
    int g1 = (color_top >> 8) & 0xFF;
    int b1 = color_top & 0xFF;
    int r2 = (color_bottom >> 16) & 0xFF;
    int g2 = (color_bottom >> 8) & 0xFF;
    int b2 = color_bottom & 0xFF;
    int denom = h > 1 ? h - 1 : 1;
    for (int row = 0; row < h; row++) {
        int rr = r1 + (r2 - r1) * row / denom;
        int gg = g1 + (g2 - g1) * row / denom;
        int bb = b1 + (b2 - b1) * row / denom;
        uint32_t color = 0x00000000 | (rr << 16) | (gg << 8) | bb;
        int x_start = x;
        int width = w;
        if (row < r) {
            int ady = r - row;
            int q = r * r - ady * ady;
            int dx = 0;
            if (q > 0) {
                while (dx * dx <= q) dx++;
                dx--;
                if (dx < 0) dx = 0;
            }
            x_start = x + r - dx;
            width = w - 2 * (r - dx);
            if (width < 0) width = 0;
        }
        if (width > 0) {
            fb_fillrect(x_start, y + row, width, 1, color);
        }
    }
}

void fb_fill_circle(int cx, int cy, int r, uint32_t color) {
    if (r <= 0 || !back_buffer) return;
    for (int dy = -r; dy <= r; dy++) {
        int q = r * r - dy * dy;
        if (q < 0) continue;
        int dx = 0;
        while (dx * dx <= q) dx++;
        dx--;
        if (dx < 0) continue;
        fb_fillrect(cx - dx, cy + dy, 2 * dx + 1, 1, color);
    }
}

void fb_box_blur(int x, int y, int w, int h, int radius) {
    if (!back_buffer || radius < 1 || w <= 0 || h <= 0) return;
    int pitch_words = (int)(fb_pitch / 4);
    int x1 = x, y1 = y;
    int x2 = x + w, y2 = y + h;
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (int)fb_width) x2 = (int)fb_width;
    if (y2 > (int)fb_height) y2 = (int)fb_height;
    int rad = radius;
    if (rad > 8) rad = 8;
    int span = rad + 1;
    for (int py = y2 - 1; py >= y1; py--) {
        for (int px = x2 - 1; px >= x1; px--) {
            int sr = 0, sg = 0, sb = 0, cnt = 0;
            for (int dy = 0; dy < span; dy++) {
                int yy = py + dy;
                if (yy >= y2) break;
                uint32_t* row = back_buffer + yy * pitch_words;
                for (int dx = 0; dx < span; dx++) {
                    int xx = px + dx;
                    if (xx >= x2) break;
                    uint32_t c = row[xx];
                    sr += (c >> 16) & 0xFF;
                    sg += (c >> 8) & 0xFF;
                    sb += c & 0xFF;
                    cnt++;
                }
            }
            if (cnt > 0) {
                uint32_t color = ((uint32_t)(sr / cnt) << 16) |
                                 ((uint32_t)(sg / cnt) << 8) |
                                 (uint32_t)(sb / cnt);
                back_buffer[py * pitch_words + px] = color;
            }
        }
    }
}

// 扫描线填充凸多边形（顶点按顺序给出）
static void fb_fill_polygon(int* px, int* py, int n, uint32_t color) {
    int ymin = py[0], ymax = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < ymin) ymin = py[i];
        if (py[i] > ymax) ymax = py[i];
    }
    for (int y = ymin; y <= ymax; y++) {
        int xints[16], cnt = 0;
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int y0 = py[i], y1 = py[j];
            int x0 = px[i], x1 = px[j];
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                int x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
                if (cnt < 16) xints[cnt++] = x;
            }
        }
        for (int a = 1; a < cnt; a++) {
            int v = xints[a], b = a - 1;
            while (b >= 0 && xints[b] > v) { xints[b + 1] = xints[b]; b--; }
            xints[b + 1] = v;
        }
        for (int k = 0; k + 1 < cnt; k += 2) {
            fb_fillrect(xints[k], y, xints[k + 1] - xints[k] + 1, 1, color);
        }
    }
}

// NEO-OS logo：蓝色六边形 + 白色旋转方块
// 基于 F:\杂七杂八\neo_os_logo.svg，256x256 视图
void fb_draw_logo(int cx, int cy, int scale) {
    int s = scale / 256;  // scale=256 为原始大小
    if (s < 1) s = 1;
    uint32_t blue = 0x001565C0;

    // 六边形顶点（SVG: 128,34 210,80 210,176 128,222 46,176 46,80）
    int px[6], py[6];
    int hx[6] = {128, 210, 210, 128, 46, 46};
    int hy[6] = {34, 80, 176, 222, 176, 80};
    for (int i = 0; i < 6; i++) {
        px[i] = cx + (hx[i] - 128) * s;
        py[i] = cy + (hy[i] - 128) * s;
    }
    // 加粗描边：绘制多个略微偏移的多边形形成粗边效果
    for (int off = 2; off >= 0; off--) {
        int ox[6], oy[6];
        for (int i = 0; i < 6; i++) {
            ox[i] = px[i];
            oy[i] = py[i];
        }
        // 简化：直接填充六边形（含描边）
        fb_fill_polygon(ox, oy, 6, blue);
    }

    // 白色旋转方块（SVG: rect 98,98 60x60 rx=14 旋转45度绕128,128）
    // 旋转45度后的正方形顶点，边长60，中心128,128，半对角线=60/2*sqrt(2)≈42
    int diag = 42 * s;  // 旋转后顶点到中心距离
    int r = 14 * s;     // 圆角
    int wx[4], wy[4];
    // 旋转45度的正方形：上下左右四个顶点
    wx[0] = cx;        wy[0] = cy - diag;  // 上
    wx[1] = cx + diag; wy[1] = cy;         // 右
    wx[2] = cx;        wy[2] = cy + diag;  // 下
    wx[3] = cx - diag; wy[3] = cy;         // 左
    // 圆角近似：用 fb_draw_circle 在四角加圆角，再填充菱形
    fb_fill_polygon(wx, wy, 4, 0x00FFFFFF);
    // 圆角：四角各画小圆
    fb_draw_circle(cx, cy - diag, r, 0x00FFFFFF);
    fb_draw_circle(cx + diag, cy, r, 0x00FFFFFF);
    fb_draw_circle(cx, cy + diag, r, 0x00FFFFFF);
    fb_draw_circle(cx - diag, cy, r, 0x00FFFFFF);
}

// 开机画面：黑色背景 + 居中 logo + "NEO ML" 文字
void fb_show_boot_logo(void) {
    if (!back_buffer) return;
    // 黑色背景
    fb_fillrect(0, 0, fb_width, fb_height, 0x00000000);
    // logo 居中，缩放到 160（约屏幕高度的 1/5）
    int scale = fb_height / 5;
    if (scale < 80) scale = 80;
    fb_draw_logo(fb_width / 2, fb_height / 2 - 40, scale);
    // "NEO OS" 文字
    draw_string_centered_bold(0, fb_height / 2 + scale / 2 + 10, fb_width, "NEO OS", 0x00FFFFFF);
    fb_blit();
}