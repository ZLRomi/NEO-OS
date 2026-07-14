#include "gui_internal.h"
#include "font_noto.h"
#include "memory.h"
#include "gfx_driver.h"
#include <stdint.h>

const uint16_t cursor_outline[16] = {
    0x0600,0x0500,0x0480,0x0440,
    0x0420,0x0410,0x0408,0x0404,
    0x0402,0x0401,0x0401,0x0420,
    0x04A0,0x0630,0x0418,0x0018,
};

const uint16_t cursor_fill[16] = {
    0x0000,0x0200,0x0300,0x0380,
    0x03C0,0x03E0,0x03F0,0x03F8,
    0x03FC,0x03FE,0x03FE,0x03C0,
    0x0240,0x0000,0x0000,0x0000,
};

static int char_width_cache[256] = { [0 ... 255] = -1 };

static inline int char_width(uint8_t c) {
    if (char_width_cache[c] < 0) {
        char_width_cache[c] = FONT_WIDTH;
    }
    return char_width_cache[c];
}

void draw_char(int x, int y, char c, uint32_t color) {
    if ((uint8_t)c > 127) return;
    if (!back_buffer) return;
    const uint8_t* glyph = font_noto[(uint8_t)c];
    int pitch_words = (int)(fb_pitch / 4);
    int cx1 = 0, cy1 = 0, cx2 = (int)fb_width, cy2 = (int)fb_height;
    if (clip_enabled) {
        cx1 = clip_x1; cy1 = clip_y1;
        cx2 = clip_x2; cy2 = clip_y2;
    }
    if (x >= cx2 || y >= cy2 || x + FONT_WIDTH <= cx1 || y + FONT_HEIGHT <= cy1) return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t byte0 = glyph[row * FONT_PITCH];
        uint8_t byte1 = glyph[row * FONT_PITCH + 1];
        uint8_t byte2 = glyph[row * FONT_PITCH + 2];
        if (!byte0 && !byte1 && !byte2) continue;
        int py = y + row;
        if (py < cy1 || py >= cy2) continue;
        uint32_t* dst = back_buffer + py * pitch_words;
        for (int col = 0; col < 8; col++) {
            if (byte0 & (0x80 >> col)) {
                int px = x + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
        for (int col = 0; col < 8; col++) {
            if (byte1 & (0x80 >> col)) {
                int px = x + 8 + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
        for (int col = 0; col < 8; col++) {
            if (byte2 & (0x80 >> col)) {
                int px = x + 16 + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
    }
}

static const uint8_t* font_cn_find(uint16_t code) {
#if FONT_CN_COUNT > 0
    int lo = 0, hi = FONT_CN_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (font_cn_codes[mid] == code) {
            return font_cn[mid];
        }
        if (font_cn_codes[mid] < code) lo = mid + 1;
        else hi = mid - 1;
    }
#endif
    return 0;
}

static int utf8_decode(const char* s, uint32_t* cp) {
    uint8_t b0 = (uint8_t)s[0];
    if (b0 < 0x80) { *cp = b0; return 1; }
    if ((b0 & 0xE0) == 0xC0) {
        *cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)s[1] & 0x3F);
        return 2;
    }
    if ((b0 & 0xF0) == 0xE0) {
        *cp = ((uint32_t)(b0 & 0x0F) << 12) |
              ((uint32_t)((uint8_t)s[1] & 0x3F) << 6) |
              ((uint8_t)s[2] & 0x3F);
        return 3;
    }
    if ((b0 & 0xF8) == 0xF0) {
        *cp = ((uint32_t)(b0 & 0x07) << 18) |
              ((uint32_t)((uint8_t)s[1] & 0x3F) << 12) |
              ((uint32_t)((uint8_t)s[2] & 0x3F) << 6) |
              ((uint8_t)s[3] & 0x3F);
        return 4;
    }
    *cp = '?';
    return 1;
}

void draw_char_cn(int x, int y, uint16_t code, uint32_t color) {
    if (!back_buffer) return;
    const uint8_t* glyph = font_cn_find(code);
    if (!glyph) {
        draw_char(x, y, '?', color);
        return;
    }
    int pitch_words = (int)(fb_pitch / 4);
    int cx1 = 0, cy1 = 0, cx2 = (int)fb_width, cy2 = (int)fb_height;
    if (clip_enabled) {
        cx1 = clip_x1; cy1 = clip_y1;
        cx2 = clip_x2; cy2 = clip_y2;
    }
    if (x >= cx2 || y >= cy2 || x + FONT_WIDTH <= cx1 || y + FONT_HEIGHT <= cy1) return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t byte0 = glyph[row * FONT_PITCH];
        uint8_t byte1 = glyph[row * FONT_PITCH + 1];
        uint8_t byte2 = glyph[row * FONT_PITCH + 2];
        if (!byte0 && !byte1 && !byte2) continue;
        int py = y + row;
        if (py < cy1 || py >= cy2) continue;
        uint32_t* dst = back_buffer + py * pitch_words;
        for (int col = 0; col < 8; col++) {
            if (byte0 & (0x80 >> col)) {
                int px = x + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
        for (int col = 0; col < 8; col++) {
            if (byte1 & (0x80 >> col)) {
                int px = x + 8 + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
        for (int col = 0; col < 8; col++) {
            if (byte2 & (0x80 >> col)) {
                int px = x + 16 + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
            }
        }
    }
}

void draw_string(int x, int y, const char* s, uint32_t color) {
    while (*s) {
        uint8_t b0 = (uint8_t)*s;
        if (b0 < 0x80) {
            draw_char(x, y, *s, color);
            x += FONT_WIDTH;
            s++;
        } else {
            uint32_t cp;
            int adv = utf8_decode(s, &cp);
            if (cp < 0x10000) {
                draw_char_cn(x, y, (uint16_t)cp, color);
            }
            x += FONT_WIDTH;
            s += adv;
        }
    }
}

void draw_char_2x(int x, int y, char c, uint32_t color) {
    if ((uint8_t)c > 127) return;
    if (!back_buffer) return;
    const uint8_t* glyph = font_noto[(uint8_t)c];
    int pitch_words = (int)(fb_pitch / 4);
    int cx1 = 0, cy1 = 0, cx2 = (int)fb_width, cy2 = (int)fb_height;
    if (clip_enabled) {
        cx1 = clip_x1; cy1 = clip_y1;
        cx2 = clip_x2; cy2 = clip_y2;
    }
    if (x >= cx2 || y >= cy2 || x + FONT_WIDTH * 2 <= cx1 || y + FONT_HEIGHT * 2 <= cy1) return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t byte0 = glyph[row * FONT_PITCH];
        uint8_t byte1 = glyph[row * FONT_PITCH + 1];
        uint8_t byte2 = glyph[row * FONT_PITCH + 2];
        if (!byte0 && !byte1 && !byte2) continue;
        int py0 = y + row * 2;
        int py1 = py0 + 1;
        uint32_t* d0 = (py0 >= cy1 && py0 < cy2) ? back_buffer + py0 * pitch_words : 0;
        uint32_t* d1 = (py1 >= cy1 && py1 < cy2) ? back_buffer + py1 * pitch_words : 0;
        uint32_t bits = ((uint32_t)byte0 << 16) | ((uint32_t)byte1 << 8) | byte2;
        for (int col = 0; col < 24; col++) {
            if (bits & (0x800000 >> col)) {
                int px0 = x + col * 2;
                int px1 = px0 + 1;
                if (d0) {
                    if (px0 >= cx1 && px0 < cx2) d0[px0] = color;
                    if (px1 >= cx1 && px1 < cx2) d0[px1] = color;
                }
                if (d1) {
                    if (px0 >= cx1 && px0 < cx2) d1[px0] = color;
                    if (px1 >= cx1 && px1 < cx2) d1[px1] = color;
                }
            }
        }
    }
}

void draw_char_bold(int x, int y, char c, uint32_t color) {
    if ((uint8_t)c > 127) return;
    if (!back_buffer) return;
    const uint8_t* glyph = font_noto[(uint8_t)c];
    int pitch_words = (int)(fb_pitch / 4);
    int cx1 = 0, cy1 = 0, cx2 = (int)fb_width, cy2 = (int)fb_height;
    if (clip_enabled) {
        cx1 = clip_x1; cy1 = clip_y1;
        cx2 = clip_x2; cy2 = clip_y2;
    }
    if (x >= cx2 || y >= cy2 || x + FONT_WIDTH <= cx1 || y + FONT_HEIGHT <= cy1) return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t byte0 = glyph[row * FONT_PITCH];
        uint8_t byte1 = glyph[row * FONT_PITCH + 1];
        uint8_t byte2 = glyph[row * FONT_PITCH + 2];
        if (!byte0 && !byte1 && !byte2) continue;
        int py = y + row;
        if (py < cy1 || py >= cy2) continue;
        uint32_t* dst = back_buffer + py * pitch_words;
        uint32_t bits = ((uint32_t)byte0 << 16) | ((uint32_t)byte1 << 8) | byte2;
        for (int col = 0; col < 24; col++) {
            if (bits & (0x800000 >> col)) {
                int px = x + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
                if (col + 1 < 24) {
                    int px2 = x + col + 1;
                    if (px2 >= cx1 && px2 < cx2) dst[px2] = color;
                }
            }
        }
    }
}

void draw_char_cn_bold(int x, int y, uint16_t code, uint32_t color) {
    if (!back_buffer) return;
    const uint8_t* glyph = font_cn_find(code);
    if (!glyph) {
        draw_char_bold(x, y, '?', color);
        return;
    }
    int pitch_words = (int)(fb_pitch / 4);
    int cx1 = 0, cy1 = 0, cx2 = (int)fb_width, cy2 = (int)fb_height;
    if (clip_enabled) {
        cx1 = clip_x1; cy1 = clip_y1;
        cx2 = clip_x2; cy2 = clip_y2;
    }
    if (x >= cx2 || y >= cy2 || x + FONT_WIDTH <= cx1 || y + FONT_HEIGHT <= cy1) return;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t byte0 = glyph[row * FONT_PITCH];
        uint8_t byte1 = glyph[row * FONT_PITCH + 1];
        uint8_t byte2 = glyph[row * FONT_PITCH + 2];
        if (!byte0 && !byte1 && !byte2) continue;
        int py = y + row;
        if (py < cy1 || py >= cy2) continue;
        uint32_t* dst = back_buffer + py * pitch_words;
        uint32_t bits = ((uint32_t)byte0 << 16) | ((uint32_t)byte1 << 8) | byte2;
        for (int col = 0; col < 24; col++) {
            if (bits & (0x800000 >> col)) {
                int px = x + col;
                if (px >= cx1 && px < cx2) dst[px] = color;
                if (col + 1 < 24) {
                    int px2 = x + col + 1;
                    if (px2 >= cx1 && px2 < cx2) dst[px2] = color;
                }
            }
        }
    }
}

void draw_string_bold(int x, int y, const char* s, uint32_t color) {
    while (*s) {
        uint8_t b0 = (uint8_t)*s;
        if (b0 < 0x80) {
            draw_char_bold(x, y, *s, color);
            x += FONT_WIDTH;
            s++;
        } else {
            uint32_t cp;
            int adv = utf8_decode(s, &cp);
            if (cp < 0x10000) {
                draw_char_cn_bold(x, y, (uint16_t)cp, color);
            }
            x += FONT_WIDTH;
            s += adv;
        }
    }
}

int utf8_char_count(const char* s) {
    int n = 0;
    while (*s) {
        uint8_t b0 = (uint8_t)*s;
        if (b0 < 0x80) { s++; n++; }
        else if ((b0 & 0xE0) == 0xC0) { s += 2; n++; }
        else if ((b0 & 0xF0) == 0xE0) { s += 3; n++; }
        else if ((b0 & 0xF8) == 0xF0) { s += 4; n++; }
        else { s++; n++; }
    }
    return n;
}

void draw_string_centered(int x, int y, int w, const char* s, uint32_t color) {
    int len = utf8_char_count(s);
    int text_w = len * FONT_WIDTH;
    draw_string(x + (w - text_w) / 2, y, s, color);
}

void draw_string_centered_bold(int x, int y, int w, const char* s, uint32_t color) {
    int len = utf8_char_count(s);
    int text_w = len * FONT_WIDTH;
    draw_string_bold(x + (w - text_w) / 2, y, s, color);
}

void int_to_str(int v, char* out) {
    char tmp[12];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) {
        tmp[i++] = '0' + (v % 10);
        v /= 10;
    }
    if (neg) tmp[i++] = '-';
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

void format_time(char* buf, int ticks) {
    int seconds = ticks / 50;
    int minutes = seconds / 60;
    int hours = minutes / 60;
    seconds %= 60;
    minutes %= 60;
    hours %= 24;
    buf[0] = '0' + hours / 10;
    buf[1] = '0' + hours % 10;
    buf[2] = ':';
    buf[3] = '0' + minutes / 10;
    buf[4] = '0' + minutes % 10;
    buf[5] = ':';
    buf[6] = '0' + seconds / 10;
    buf[7] = '0' + seconds % 10;
    buf[8] = '\0';
}

int string_width(const char* s) {
    int len = utf8_char_count(s);
    return len * FONT_WIDTH;
}

void draw_string_wrapped(int x, int y, int max_w, int max_h, const char* s, uint32_t color) {
    int cx = 0, cy = 0;
    int cols = max_w / FONT_WIDTH;
    if (cols <= 0) return;
    int max_rows = max_h / FONT_HEIGHT;
    while (*s && cy < max_rows) {
        uint8_t b0 = (uint8_t)*s;
        if (b0 < 0x80) {
            char c = *s++;
            if (c == '\n') {
                cx = 0;
                cy++;
            } else {
                int cw = char_width((uint8_t)c);
                if (cx >= cols) {
                    cx = 0;
                    cy++;
                    if (cy >= max_rows) break;
                }
                draw_char(x + cx * FONT_WIDTH, y + cy * FONT_HEIGHT, c, color);
                cx += (cw + FONT_WIDTH - 1) / FONT_WIDTH;
            }
        } else {
            uint32_t cp;
            int adv = utf8_decode(s, &cp);
            if (cx >= cols) {
                cx = 0;
                cy++;
                if (cy >= max_rows) break;
            }
            if (cp < 0x10000) draw_char_cn(x + cx * FONT_WIDTH, y + cy * FONT_HEIGHT, (uint16_t)cp, color);
            cx++;
            s += adv;
        }
    }
}

void draw_cursor(int x, int y) {
    for (int row = 0; row < 16; row++) {
        uint16_t outline = cursor_outline[row];
        uint16_t fill = cursor_fill[row];
        for (int col = 0; col < 11; col++) {
            uint16_t mask = 0x0400 >> col;
            if (outline & mask) {
                fb_putpixel(x + col, y + row, 0x00000000);
            } else if (fill & mask) {
                fb_putpixel(x + col, y + row, 0x00FFFFFF);
            }
        }
    }
}

void cursor_draw_front(int x, int y) {
    uint32_t* front = (uint32_t*)gfx_get_fb();
    if (!front) return;
    int pitch = (int)(fb_pitch / 4);
    for (int row = 0; row < CURSOR_H; row++) {
        int fy = y + row;
        if (fy < 0 || fy >= (int)fb_height) continue;
        uint16_t outline = cursor_outline[row];
        uint16_t fill = cursor_fill[row];
        for (int col = 0; col < CURSOR_W; col++) {
            int fx = x + col;
            if (fx < 0 || fx >= (int)fb_width) continue;
            uint16_t mask = 0x0400 >> col;
            if (outline & mask) {
                front[fy * pitch + fx] = 0x00000000;
            } else if (fill & mask) {
                front[fy * pitch + fx] = 0x00FFFFFF;
            }
        }
    }
}

static uint32_t cursor_bg_save[CURSOR_W * CURSOR_H];
static int cursor_bg_saved = 0;
static int cursor_bg_x = 0;
static int cursor_bg_y = 0;

void cursor_save_bg(int x, int y) {
    uint32_t* front = (uint32_t*)gfx_get_fb();
    if (!front) return;
    int pitch = (int)(fb_pitch / 4);
    for (int row = 0; row < CURSOR_H; row++) {
        int fy = y + row;
        if (fy < 0 || fy >= (int)fb_height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int fx = x + col;
            if (fx < 0 || fx >= (int)fb_width) continue;
            cursor_bg_save[row * CURSOR_W + col] = front[fy * pitch + fx];
        }
    }
    cursor_bg_x = x;
    cursor_bg_y = y;
    cursor_bg_saved = 1;
}

void cursor_restore_bg(void) {
    if (!cursor_bg_saved) return;
    uint32_t* front = (uint32_t*)gfx_get_fb();
    if (!front) return;
    int pitch = (int)(fb_pitch / 4);
    for (int row = 0; row < CURSOR_H; row++) {
        int fy = cursor_bg_y + row;
        if (fy < 0 || fy >= (int)fb_height) continue;
        for (int col = 0; col < CURSOR_W; col++) {
            int fx = cursor_bg_x + col;
            if (fx < 0 || fx >= (int)fb_width) continue;
            front[fy * pitch + fx] = cursor_bg_save[row * CURSOR_W + col];
        }
    }
    cursor_bg_saved = 0;
}
