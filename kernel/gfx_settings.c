#include "gfx_settings.h"
#include "gfx_driver.h"
#include "gui_internal.h"
#include "memory.h"
#include <stdint.h>

static int settings_open = 0;

#define RES_COUNT 5
static const struct { int w, h; const char* label; } resolutions[RES_COUNT] = {
    {  640,  480, "640 x 480"   },
    {  800,  600, "800 x 600"   },
    { 1024,  768, "1024 x 768"  },
    { 1280,  720, "1280 x 720"  },
    { 1920, 1080, "1920 x 1080" },
};

#define PANEL_W 300
#define PANEL_H 230
#define TITLE_H 24
#define BTN_W 260
#define BTN_H 28
#define BTN_GAP 4
#define BTN_X_OFF 20
#define BTN_Y_OFF 36

static void panel_pos(int* px, int* py) {
    int sw = (int)fb_width;
    int sh = (int)fb_height;
    *px = (sw - PANEL_W) / 2;
    *py = (sh - PANEL_H) / 2;
    if (*px < 0) *px = 0;
    if (*py < 0) *py = 0;
}

void gfx_settings_open(void) {
    settings_open = 1;
    gui_dirty = 1;
}

void gfx_settings_close(void) {
    settings_open = 0;
    gui_dirty = 1;
}

int gfx_settings_is_open(void) {
    return settings_open;
}

static int is_current(int w, int h) {
    return (gfx_get_width() == w && gfx_get_height() == h);
}

void gfx_settings_render(void) {
    if (!settings_open) return;
    int px, py;
    panel_pos(&px, &py);

    fb_fillrect(px, py, PANEL_W, PANEL_H, COLOR_WINDOW_BODY);
    fb_draw_rect_outline(px, py, PANEL_W, PANEL_H, 0x00888888);

    fb_fillrect(px, py, PANEL_W, TITLE_H, 0x002E7D32);
    draw_string_bold(px + 8, py + 6, "Display Settings", COLOR_TEXT_WHITE);
    fb_fillrect(px + PANEL_W - 22, py + 2, 20, 20, COLOR_CLOSE_BTN);
    draw_string(px + PANEL_W - 17, py + 6, "X", COLOR_TEXT_WHITE);

    int cur_w = gfx_get_width();
    int cur_h = gfx_get_height();
    char cur_buf[48];
    int j = 0;
    const char* p = "Current: ";
    while (*p && j < 46) cur_buf[j++] = *p++;
    int_to_str(cur_w, cur_buf + j);
    while (cur_buf[j] && j < 46) j++;
    if (j < 45) { cur_buf[j++] = 'x'; }
    int_to_str(cur_h, cur_buf + j);
    draw_string(px + 20, py + PANEL_H - 20, cur_buf, COLOR_TEXT_GRAY);

    int by = py + BTN_Y_OFF;
    for (int i = 0; i < RES_COUNT; i++) {
        uint32_t bg = is_current(resolutions[i].w, resolutions[i].h) ? 0x004466AA : 0x00CCCCCC;
        fb_fillrect(px + BTN_X_OFF, by, BTN_W, BTN_H, bg);
        fb_draw_rect_outline(px + BTN_X_OFF, by, BTN_W, BTN_H, 0x00666666);
        draw_string_bold(px + BTN_X_OFF + 12, by + 7, resolutions[i].label, COLOR_TEXT_WHITE);
        if (is_current(resolutions[i].w, resolutions[i].h)) {
            draw_string_bold(px + BTN_X_OFF + BTN_W - 24, by + 7, "*", COLOR_TEXT_WHITE);
        }
        by += BTN_H + BTN_GAP;
    }
}

int gfx_settings_handle_click(int x, int y) {
    if (!settings_open) return 0;

    int px, py;
    panel_pos(&px, &py);

    if (x < px || x >= px + PANEL_W || y < py || y >= py + PANEL_H) {
        gfx_settings_close();
        return 1;
    }

    if (x >= px + PANEL_W - 22 && x < px + PANEL_W - 2 &&
        y >= py + 2 && y < py + 22) {
        gfx_settings_close();
        return 1;
    }

    int by = py + BTN_Y_OFF;
    for (int i = 0; i < RES_COUNT; i++) {
        if (x >= px + BTN_X_OFF && x < px + BTN_X_OFF + BTN_W &&
            y >= by && y < by + BTN_H) {
            int rw = resolutions[i].w;
            int rh = resolutions[i].h;
            if (!is_current(rw, rh)) {
                if (gfx_set_mode(rw, rh, 32)) {
                    fb_free();
                    fb_init();
                    for (int k = 0; k < window_count; k++) {
                        clamp_window(&windows[k]);
                    }
                }
            }
            gfx_settings_close();
            return 1;
        }
        by += BTN_H + BTN_GAP;
    }

    return 1;
}
