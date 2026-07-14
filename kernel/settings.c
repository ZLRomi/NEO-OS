#include "gui_internal.h"
#include "settings.h"
#include "theme.h"
#include "audio.h"
#include "wallpaper.h"
#include "gfx_driver.h"
#include "gfx_settings.h"
#include "fat32.h"
#include "auth.h"
#include "mouse.h"
#include <stdint.h>
#include <stddef.h>

#define SETTINGS_CATEGORIES 5
#define SETTINGS_NAV_W 140
#define SETTINGS_NAV_BTN_H 36
#define SETTINGS_NAV_BTN_GAP 4
#define SETTINGS_NAV_Y 12
#define SETTINGS_NAV_X 8

typedef enum {
    SET_CAT_DISPLAY = 0,
    SET_CAT_APPEARANCE,
    SET_CAT_SOUND,
    SET_CAT_USER,
    SET_CAT_ABOUT,
} set_cat_t;

typedef struct {
    int category;
    int volume;
    int muted;
    int dragging_volume;
    int res_index;
} settings_state_t;

static settings_state_t st;

static const char* category_labels[SETTINGS_CATEGORIES] = {
    "显示", "外观", "声音", "用户", "关于"
};

#define RES_COUNT 5
static const struct { int w, h; const char* label; } resolutions[RES_COUNT] = {
    {  640,  480, "640 x 480"   },
    {  800,  600, "800 x 600"   },
    { 1024,  768, "1024 x 768"  },
    { 1280,  720, "1280 x 720"  },
    { 1920, 1080, "1920 x 1080" },
};

static int is_current_res(int w, int h) {
    return gfx_get_width() == w && gfx_get_height() == h;
}

void settings_init(void) {
    st.category = SET_CAT_DISPLAY;
    st.volume = audio_get_volume();
    st.muted = 0;
    st.dragging_volume = 0;
    st.res_index = -1;
    for (int i = 0; i < RES_COUNT; i++) {
        if (is_current_res(resolutions[i].w, resolutions[i].h)) {
            st.res_index = i;
            break;
        }
    }
}

static void render_nav(window_t* win) {
    int x = win->x + 8;
    int y = win->y + 32 + SETTINGS_NAV_Y;
    int w = SETTINGS_NAV_W;
    for (int i = 0; i < SETTINGS_CATEGORIES; i++) {
        int by = y + i * (SETTINGS_NAV_BTN_H + SETTINGS_NAV_BTN_GAP);
        uint32_t bg = (i == st.category) ? g_theme.accent : g_theme.title_bar_bottom;
        fb_fillrect(x, by, w, SETTINGS_NAV_BTN_H, bg);
        fb_draw_rect_outline(x, by, w, SETTINGS_NAV_BTN_H, g_theme.border);
        draw_string(x + 12, by + 10, category_labels[i], COLOR_TEXT_WHITE);
    }
}

static void render_panel_header(int px, int py, const char* title) {
    draw_string_bold(px, py, title, g_theme.text);
    fb_fillrect(px, py + 22, 360, 1, g_theme.border);
}

static void render_display_panel(window_t* win, int px, int py) {
    (void)win;
    render_panel_header(px, py, "显示");
    int by = py + 36;
    draw_string(px, by, "分辨率：", g_theme.text);
    by += 24;
    for (int i = 0; i < RES_COUNT; i++) {
        uint32_t bg = is_current_res(resolutions[i].w, resolutions[i].h)
            ? g_theme.accent : 0x00CCCCCC;
        fb_fillrect(px, by, 200, 24, bg);
        fb_draw_rect_outline(px, by, 200, 24, 0x00666666);
        draw_string(px + 12, by + 4, resolutions[i].label, COLOR_TEXT_WHITE);
        if (is_current_res(resolutions[i].w, resolutions[i].h)) {
            draw_string(px + 178, by + 4, "*", COLOR_TEXT_WHITE);
        }
        by += 28;
    }
    by += 8;
    char info[64];
    int j = 0;
    const char* p = "刷新率：60 Hz";
    while (*p && j < 60) info[j++] = *p++;
    info[j] = 0;
    draw_string(px, by, info, g_theme.text_secondary);
}

static void render_appearance_panel(window_t* win, int px, int py) {
    (void)win;
    render_panel_header(px, py, "外观");
    int by = py + 36;
    draw_string(px, by, "主题：", g_theme.text);
    fb_fillrect(px + 80, by - 4, 80, 28, g_theme.accent);
    fb_draw_rect_outline(px + 80, by - 4, 80, 28, 0x00666666);
    draw_string_centered(px + 80, by + 2, 80, "浅色", COLOR_TEXT_WHITE);
    fb_fillrect(px + 168, by - 4, 80, 28, 0x00888888);
    fb_draw_rect_outline(px + 168, by - 4, 80, 28, 0x00666666);
    draw_string_centered(px + 168, by + 2, 80, "深色", COLOR_TEXT_WHITE);

    by += 40;
    draw_string(px, by, "壁纸：", g_theme.text);
    fb_fillrect(px + 100, by - 4, 100, 28, g_theme.accent);
    fb_draw_rect_outline(px + 100, by - 4, 100, 28, 0x00666666);
    draw_string_centered(px + 100, by + 2, 100, "更换", COLOR_TEXT_WHITE);

    by += 40;
    draw_string(px, by, "强调色：", g_theme.text);
    fb_fillrect(px + 80, by, 28, 28, g_theme.accent);
    fb_draw_rect_outline(px + 80, by, 28, 28, g_theme.border);
    char hex[12];
    int k = 0;
    const char* hp = "0x";
    while (*hp && k < 10) hex[k++] = *hp++;
    uint32_t ac = g_theme.accent;
    const char* hexd = "0123456789ABCDEF";
    for (int i = 5; i >= 0; i--) hex[k++] = hexd[(ac >> (i * 4)) & 0xF];
    hex[k] = 0;
    draw_string(px + 120, by + 4, hex, g_theme.text);
}

static void render_sound_panel(window_t* win, int px, int py) {
    (void)win;
    render_panel_header(px, py, "声音");
    int by = py + 36;
    draw_string(px, by, "音量：", g_theme.text);
    int slider_x = px + 90;
    int slider_y = by + 8;
    int slider_w = 200;
    fb_fillrect(slider_x, slider_y, slider_w, 6, 0x00AAAAAA);
    int knob_x = slider_x + (st.volume * (slider_w - 12) / 100);
    fb_fillrect(knob_x, slider_y - 6, 12, 18, g_theme.accent);
    fb_draw_rect_outline(knob_x, slider_y - 6, 12, 18, 0x00444444);
    char vbuf[12];
    int_to_str(st.volume, vbuf);
    draw_string(slider_x + slider_w + 10, by, vbuf, g_theme.text);

    by += 40;
    draw_string(px, by, "静音：", g_theme.text);
    int sw_x = px + 90;
    int sw_y = by - 2;
    int sw_w = 50;
    int sw_h = 22;
    uint32_t sw_bg = st.muted ? 0x00CC4444 : 0x006699CC;
    fb_fillrect(sw_x, sw_y, sw_w, sw_h, sw_bg);
    fb_draw_rect_outline(sw_x, sw_y, sw_w, sw_h, 0x00666666);
    int kx = st.muted ? sw_x + sw_w - 22 : sw_x + 4;
    fb_fillrect(kx, sw_y + 2, 18, sw_h - 4, COLOR_TEXT_WHITE);
    draw_string(sw_x + sw_w + 10, by, st.muted ? "已静音" : "开启", g_theme.text);
}

static void render_user_panel(window_t* win, int px, int py) {
    (void)win;
    render_panel_header(px, py, "用户");
    int by = py + 36;
    draw_string(px, by, "用户名：", g_theme.text);
    const char* uname = auth_get_current_username();
    draw_string(px + 100, by, uname ? uname : "（无）", g_theme.text);
    by += 32;
    draw_string(px, by, "新密码：", g_theme.text);
    fb_fillrect(px + 130, by - 4, 200, 24, COLOR_TEXT_WHITE);
    fb_draw_rect_outline(px + 130, by - 4, 200, 24, 0x00888888);
    by += 36;
    fb_fillrect(px, by, 100, 24, g_theme.accent);
    fb_draw_rect_outline(px, by, 100, 24, 0x00666666);
    draw_string_centered(px, by + 4, 100, "应用", COLOR_TEXT_WHITE);
}

static void render_about_panel(window_t* win, int px, int py) {
    (void)win;
    render_panel_header(px, py, "关于");
    int by = py + 36;
    draw_string_bold(px, by, "NEO-OS 1.0", g_theme.text);
    by += 28;
    draw_string(px, by, "构建：2025-08", g_theme.text_secondary);
    by += 22;
    draw_string(px, by, "作者：NEO Team", g_theme.text_secondary);
    by += 22;
    draw_string(px, by, "许可证：MIT", g_theme.text_secondary);
    by += 22;
    draw_string(px, by, "32 位 x86 内核", g_theme.text_secondary);
}

void render_settings(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, 8, g_theme.bg);
    render_title_bar(win, g_theme.title_bar);

    render_nav(win);

    int px = win->x + SETTINGS_NAV_X + SETTINGS_NAV_W + 16;
    int py = win->y + 32 + 16;
    int pw = w - (SETTINGS_NAV_X + SETTINGS_NAV_W + 16) - 12;
    (void)pw;
    (void)h;

    if (st.category == SET_CAT_DISPLAY) render_display_panel(win, px, py);
    else if (st.category == SET_CAT_APPEARANCE) render_appearance_panel(win, px, py);
    else if (st.category == SET_CAT_SOUND) render_sound_panel(win, px, py);
    else if (st.category == SET_CAT_USER) render_user_panel(win, px, py);
    else if (st.category == SET_CAT_ABOUT) render_about_panel(win, px, py);
}

void settings_handle_click(window_t* win, int mx, int my) {
    int nav_x = win->x + 8;
    int nav_y = win->y + 32 + SETTINGS_NAV_Y;
    if (mx >= nav_x && mx < nav_x + SETTINGS_NAV_W) {
        int idx = (my - nav_y) / (SETTINGS_NAV_BTN_H + SETTINGS_NAV_BTN_GAP);
        int in_btn = (my - nav_y) % (SETTINGS_NAV_BTN_H + SETTINGS_NAV_BTN_GAP);
        if (idx >= 0 && idx < SETTINGS_CATEGORIES && in_btn < SETTINGS_NAV_BTN_H) {
            st.category = idx;
            gui_dirty = 1;
            return;
        }
    }

    int px = win->x + SETTINGS_NAV_X + SETTINGS_NAV_W + 16;
    int py = win->y + 32 + 16;

    if (st.category == SET_CAT_DISPLAY) {
        int by = py + 36 + 24;
        for (int i = 0; i < RES_COUNT; i++) {
            if (mx >= px && mx < px + 200 && my >= by && my < by + 24) {
                int rw = resolutions[i].w;
                int rh = resolutions[i].h;
                if (!is_current_res(rw, rh)) {
                    if (gfx_set_mode(rw, rh, 32)) {
                        fb_free();
                        fb_init();
                        for (int k = 0; k < window_count; k++) {
                            clamp_window(&windows[k]);
                        }
                    }
                }
                gui_dirty = 1;
                return;
            }
            by += 28;
        }
    } else if (st.category == SET_CAT_APPEARANCE) {
        int by = py + 36;
        if (my >= by - 4 && my < by + 24) {
            if (mx >= px + 80 && mx < px + 160) {
                theme_set_light();
                gui_dirty = 1;
                return;
            }
            if (mx >= px + 168 && mx < px + 248) {
                theme_set_dark();
                gui_dirty = 1;
                return;
            }
        }
        by += 40;
        if (my >= by - 4 && my < by + 24 && mx >= px + 100 && mx < px + 200) {
            static const char* candidates[] = {
                "/wallpaper.bmp", "/wallpaper.jpg", "/wallpaper.jpeg",
                "/etc/wallpaper.bmp", "/etc/wallpaper.jpg"
            };
            int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
            for (int i = 0; i < n; i++) {
                if (wallpaper_load(candidates[i], g_wallpaper_mode) == 0) {
                    wallpaper_save_config();
                    break;
                }
            }
            gui_dirty = 1;
            return;
        }
    } else if (st.category == SET_CAT_SOUND) {
        int by = py + 36;
        int slider_x = px + 90;
        int slider_y = by + 8;
        int slider_w = 200;
        uint8_t buttons = mouse_buttons;
        if ((mx >= slider_x - 4 && mx < slider_x + slider_w + 4 &&
             my >= slider_y - 8 && my < slider_y + 14) ||
            (st.dragging_volume && (buttons & 0x01))) {
            int v = (mx - slider_x) * 100 / (slider_w - 12);
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            st.volume = v;
            st.muted = 0;
            audio_set_master_volume(v);
            st.dragging_volume = (buttons & 0x01) ? 1 : 0;
            gui_dirty = 1;
            return;
        }
        if (!buttons) st.dragging_volume = 0;
        by += 40;
        int sw_x = px + 90;
        int sw_y = by - 2;
        if (mx >= sw_x && mx < sw_x + 50 && my >= sw_y && my < sw_y + 22) {
            st.muted = !st.muted;
            audio_set_master_volume(st.muted ? 0 : st.volume);
            gui_dirty = 1;
            return;
        }
    } else if (st.category == SET_CAT_USER) {
        int by = py + 36 + 32;
        if (mx >= px + 130 && mx < px + 330 && my >= by - 4 && my < by + 20) {
            gui_dirty = 1;
            return;
        }
        by += 36;
        if (mx >= px && mx < px + 100 && my >= by && my < by + 24) {
            gshell_puts("[Settings] Password change requested\n");
            gui_dirty = 1;
            return;
        }
    }
}

void settings_update_hover(int mx, int my) {
    (void)mx;
    (void)my;
}
