#include "gui_internal.h"
#include "theme.h"
#include <stdint.h>

desktop_icon_t desktop_icons[DESKTOP_ICON_COUNT];

void blend_round_rect(int x, int y, int w, int h, int r, uint32_t color) {
    if (!back_buffer) return;
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    uint32_t a = color >> 24;
    if (a == 0) return;
    int pitch_words = (int)(fb_pitch / 4);
    uint32_t inv = 255 - a;
    uint32_t sr = (color >> 16) & 0xFF;
    uint32_t sg = (color >> 8) & 0xFF;
    uint32_t sb = color & 0xFF;
    int row, col;
    for (row = 0; row < h; row++) {
        int py = y + row;
        if (py < 0 || py >= (int)fb_height) continue;
        for (col = 0; col < w; col++) {
            int px = x + col;
            if (px < 0 || px >= (int)fb_width) continue;
            if (r > 0) {
                int in_corner = 0;
                int cx = 0, cy = 0;
                if (col < r && row < r) { cx = r; cy = r; in_corner = 1; }
                else if (col >= w - r && row < r) { cx = w - r - 1; cy = r; in_corner = 1; }
                else if (col < r && row >= h - r) { cx = r; cy = h - r - 1; in_corner = 1; }
                else if (col >= w - r && row >= h - r) { cx = w - r - 1; cy = h - r - 1; in_corner = 1; }
                if (in_corner) {
                    int dx = col - cx;
                    int dy = row - cy;
                    if (dx * dx + dy * dy > r * r) continue;
                }
            }
            uint32_t* dst = back_buffer + py * pitch_words + px;
            uint32_t dp = *dst;
            uint32_t dr = (dp >> 16) & 0xFF;
            uint32_t dg = (dp >> 8) & 0xFF;
            uint32_t db = dp & 0xFF;
            uint32_t rr = (sr * a + dr * inv) / 255;
            uint32_t gg = (sg * a + dg * inv) / 255;
            uint32_t bb = (sb * a + db * inv) / 255;
            *dst = (rr << 16) | (gg << 8) | bb;
        }
    }
}

void init_desktop_icons(void) {
    int start_y = 20;
    int x = 20;
    int y = start_y;
    int max_h = DESKTOP_ICON_SIZE + DESKTOP_LABEL_H + 8;

    desktop_icons[0].type = WINDOW_TYPE_GSHELL; desktop_icons[0].label = "终端";
    desktop_icons[1].type = WINDOW_TYPE_DEMO;   desktop_icons[1].label = "演示";
    desktop_icons[2].type = WINDOW_TYPE_ABOUT;  desktop_icons[2].label = "关于";
    desktop_icons[3].type = WINDOW_TYPE_CALCULATOR; desktop_icons[3].label = "计算器";
    desktop_icons[4].type = WINDOW_TYPE_BROWSER; desktop_icons[4].label = "浏览器";
    desktop_icons[5].type = WINDOW_TYPE_FILE_MANAGER; desktop_icons[5].label = "文件";
    desktop_icons[6].type = WINDOW_TYPE_SETTINGS; desktop_icons[6].label = "设置";

    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        desktop_icons[i].x = x;
        desktop_icons[i].y = y;
        desktop_icons[i].selected = 0;
        desktop_icons[i].hovered = 0;
        y += max_h + DESKTOP_ICON_SPACING;
        if (y + max_h > (int)fb_height - TASKBAR_HEIGHT - 20) {
            y = start_y;
            x += DESKTOP_ICON_SIZE + 80;
        }
    }
}

static int icon_label_width(const char* s) {
    int len = 0;
    while (s[len]) len++;
    if (len * FONT_WIDTH > DESKTOP_ICON_SIZE + 24) return DESKTOP_ICON_SIZE + 24;
    return len * FONT_WIDTH;
}

static void draw_desktop_icon(int idx, int mx, int my) {
    desktop_icon_t* icon = &desktop_icons[idx];
    int x = icon->x;
    int y = icon->y;
    int label_w = icon_label_width(icon->label);
    int box_w = label_w > DESKTOP_ICON_SIZE ? label_w + 8 : DESKTOP_ICON_SIZE + 8;
    int box_x = x - (box_w - DESKTOP_ICON_SIZE) / 2;
    int box_h = DESKTOP_ICON_SIZE + DESKTOP_LABEL_H + 8;

    icon->hovered = (mx >= box_x && mx < box_x + box_w && my >= y - 4 && my < y - 4 + box_h);

    if (icon->selected) {
        uint32_t accent = (g_theme.accent & 0x00FFFFFF) | 0x66000000u;
        blend_round_rect(box_x, y - 4, box_w, box_h, 6, accent);
    } else if (icon->hovered) {
        blend_round_rect(box_x, y - 4, box_w, box_h, 6, 0x33FFFFFFu);
    }

    if (icon->type == WINDOW_TYPE_GSHELL) draw_icon_terminal(x, y);
    else if (icon->type == WINDOW_TYPE_DEMO) draw_icon_demo(x, y);
    else if (icon->type == WINDOW_TYPE_ABOUT) draw_icon_about(x, y);
    else if (icon->type == WINDOW_TYPE_CALCULATOR) draw_icon_calc(x, y);
    else if (icon->type == WINDOW_TYPE_BROWSER) draw_icon_browser(x, y);
    else if (icon->type == WINDOW_TYPE_FILE_MANAGER) {
        fb_fillrect(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, g_theme.accent);
        fb_draw_rect_outline(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, g_theme.border);
        draw_icon_folder(x + 4, y + 4);
        draw_string_bold(x + 6, y + 22, "FM", COLOR_TEXT_WHITE);
    } else if (icon->type == WINDOW_TYPE_SETTINGS) {
        draw_icon_settings(x, y);
        fb_fillrect(x, y, 16, 16, g_theme.accent);
        draw_icon_settings(x + 16, y + 16);
    }

    int text_x = x + (DESKTOP_ICON_SIZE - label_w) / 2;
    int label_y = y + DESKTOP_ICON_SIZE + 4;
    if (icon->selected) {
        draw_string_bold(text_x + 1, label_y + 1, icon->label, 0x00202020);
    }
    draw_string_bold(text_x, label_y, icon->label, COLOR_TEXT_WHITE);
}

void render_desktop_icons(int mx, int my) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        draw_desktop_icon(i, mx, my);
    }
}

void desktop_icon_open(int type) {
    window_t* win = window_find_by_type(type);
    if (!win || win->visible) return;
    int ex = win->normal_x, ey = win->normal_y, ew = win->normal_w, eh = win->normal_h;
    int sx = win->x + win->w / 2 - 10;
    int sy = win->y + win->h / 2 - 10;
    window_bring_to_front_internal(win);
    window_start_animation(win, 12, sx, sy, 20, 20, ex, ey, ew, eh, 0);
}

void desktop_icon_handle_click(int mx, int my) {
    for (int i = 0; i < DESKTOP_ICON_COUNT; i++) {
        desktop_icon_t* icon = &desktop_icons[i];
        int label_w = icon_label_width(icon->label);
        int box_w = label_w > DESKTOP_ICON_SIZE ? label_w + 8 : DESKTOP_ICON_SIZE + 8;
        int box_x = icon->x - (box_w - DESKTOP_ICON_SIZE) / 2;
        int box_h = DESKTOP_ICON_SIZE + DESKTOP_LABEL_H + 8;
        if (mx >= box_x && mx < box_x + box_w && my >= icon->y - 4 && my < icon->y - 4 + box_h) {
            for (int j = 0; j < DESKTOP_ICON_COUNT; j++) desktop_icons[j].selected = 0;
            icon->selected = 1;
            desktop_icon_open(icon->type);
            return;
        }
    }
    for (int j = 0; j < DESKTOP_ICON_COUNT; j++) desktop_icons[j].selected = 0;
}
