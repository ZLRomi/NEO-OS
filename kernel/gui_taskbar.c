#include "gui_internal.h"
#include "theme.h"
#include "idt.h"
#include <stdint.h>

static int taskbar_hover_index = -1;
static int taskbar_hover_ticks = 0;
static int taskbar_clock_hovered = 0;
static char time_buf[16];

int taskbar_start_btn_x(void) { return 8; }
int taskbar_start_btn_y(void) { return (int)fb_height - TASKBAR_HEIGHT + (TASKBAR_HEIGHT - START_BTN_SIZE) / 2; }
int taskbar_list_y(void)      { return (int)fb_height - TASKBAR_HEIGHT + (TASKBAR_HEIGHT - TASKBAR_BTN_H) / 2; }
int taskbar_button_y(void)    { return taskbar_list_y(); }

int taskbar_list_start(void) {
    int sw = (int)fb_width;
    int n = window_count;
    if (n <= 0) return sw / 2;
    int total = n * TASKBAR_BTN_W + (n - 1) * TASKBAR_BTN_GAP;
    return sw / 2 - total / 2;
}

int taskbar_button_x(int idx) {
    return taskbar_list_start() + idx * (TASKBAR_BTN_W + TASKBAR_BTN_GAP);
}

int taskbar_tray_x(void) {
    return (int)fb_width - 96;
}

int taskbar_start_btn_hit(int mx, int my) {
    int x = taskbar_start_btn_x();
    int y = taskbar_start_btn_y();
    return (mx >= x && mx < x + START_BTN_SIZE && my >= y && my < y + START_BTN_SIZE);
}

int taskbar_button_at(int mx, int my) {
    int ly = taskbar_list_y();
    if (my < ly || my >= ly + TASKBAR_BTN_H) return -1;
    int start = taskbar_list_start();
    for (int i = 0; i < window_count; i++) {
        int bx = start + i * (TASKBAR_BTN_W + TASKBAR_BTN_GAP);
        if (mx >= bx && mx < bx + TASKBAR_BTN_W) return i;
    }
    return -1;
}

int taskbar_clock_hit(int mx, int my) {
    int sw = (int)fb_width;
    int ty = (int)fb_height - TASKBAR_HEIGHT;
    int time_w = string_width(time_buf);
    int bw_w = time_w + 16;
    int cx = sw - bw_w - 12;
    int cy = ty + (TASKBAR_HEIGHT - 24) / 2;
    return (mx >= cx && mx < cx + bw_w && my >= cy && my < cy + 24);
}

int taskbar_clock_hover(void) { return taskbar_clock_hovered; }

static void render_start_button(int x, int y, int hovered) {
    if (hovered) blend_round_rect(x - 4, y - 4, START_BTN_SIZE + 8, START_BTN_SIZE + 8, 6, 0x33FFFFFFu);
    int q = START_BTN_SIZE / 2 - 3;
    int ox = x + 4;
    int oy = y + 4;
    int gap = 2;
    fb_fillrect(ox, oy, q, q, COLOR_START_WIN_RED);
    fb_fillrect(ox + q + gap, oy, q, q, COLOR_START_WIN_GREEN);
    fb_fillrect(ox, oy + q + gap, q, q, COLOR_START_WIN_BLUE);
    fb_fillrect(ox + q + gap, oy + q + gap, q, q, COLOR_START_WIN_YELLOW);
}

void render_taskbar(int mx, int my) {
    int sw = (int)fb_width;
    int sh = (int)fb_height;
    int ty = sh - TASKBAR_HEIGHT;

    fb_box_blur(0, ty, sw, TASKBAR_HEIGHT, 4);
    uint32_t tb_color = (g_theme.bg & 0x00FFFFFF) | 0xC8000000u;
    fb_fillrect_alpha(0, ty, sw, TASKBAR_HEIGHT, tb_color);

    int start_x = taskbar_start_btn_x();
    int start_y = taskbar_start_btn_y();
    int start_hovered = taskbar_start_btn_hit(mx, my);
    render_start_button(start_x, start_y, start_hovered);

    int list_y = taskbar_list_y();
    int active_idx = -1;
    window_t* top = window_top_visible();
    if (top) {
        for (int i = 0; i < window_count; i++) {
            if (&windows[i] == top) { active_idx = i; break; }
        }
    }

    taskbar_hover_index = -1;
    for (int i = 0; i < window_count; i++) {
        int lx = taskbar_button_x(i);
        int hovered = (mx >= lx && mx < lx + TASKBAR_BTN_W &&
                       my >= list_y && my < list_y + TASKBAR_BTN_H);
        int is_active = (i == active_idx) && !windows[i].minimized;
        int is_minimized = windows[i].minimized;

        if (hovered) {
            taskbar_hover_index = i;
            taskbar_hover_ticks = (int)timer_ticks;
            blend_round_rect(lx, list_y, TASKBAR_BTN_W, TASKBAR_BTN_H, 6, 0x28FFFFFFu);
        } else if (is_active) {
            blend_round_rect(lx, list_y, TASKBAR_BTN_W, TASKBAR_BTN_H, 6, 0x18FFFFFFu);
        }

        int ix = lx + (TASKBAR_BTN_W - 16) / 2;
        int iy = list_y + (TASKBAR_BTN_H - 16) / 2;
        if (windows[i].type == WINDOW_TYPE_GSHELL) draw_icon_terminal_small(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_DEMO) draw_icon_demo_small(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_ABOUT) draw_icon_about_small(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_CALCULATOR) draw_icon_calc_small(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_FILE_MANAGER) draw_icon_folder_small(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_SETTINGS) draw_icon_settings(ix, iy);
        else if (windows[i].type == WINDOW_TYPE_BROWSER) draw_icon_browser_small(ix, iy);

        if ((windows[i].visible || is_minimized) && !is_minimized) {
            int ul_w = 16;
            int ul_h = 3;
            int ul_x = lx + (TASKBAR_BTN_W - ul_w) / 2;
            int ul_y = list_y + TASKBAR_BTN_H - ul_h - 1;
            if (is_active) {
                fb_fillrect(ul_x, ul_y, ul_w, ul_h, g_theme.accent);
            } else {
                uint32_t dim = (g_theme.accent & 0x00FFFFFF) | 0x66000000u;
                fb_fillrect_alpha(ul_x, ul_y, ul_w, ul_h, dim);
            }
        }
    }

    format_time(time_buf, (int)timer_ticks);
    int time_w = string_width(time_buf);
    int bw_w = time_w + 16;
    int cx = sw - bw_w - 12;
    int cy = ty + (TASKBAR_HEIGHT - 24) / 2;
    taskbar_clock_hovered = (mx >= cx && mx < cx + bw_w && my >= cy && my < cy + 24);
    if (taskbar_clock_hovered) {
        blend_round_rect(cx, cy, bw_w, 24, 4, 0x28FFFFFFu);
    }
    draw_string_bold(cx + 8, cy + (24 - 16) / 2, time_buf, COLOR_TEXT_WHITE);

    int tray_x = cx - 12;
    fb_draw_circle(tray_x - 8, ty + TASKBAR_HEIGHT / 2, 5, 0x00CCCCCC);
    fb_draw_circle(tray_x - 24, ty + TASKBAR_HEIGHT / 2, 5, 0x00CCCCCC);
    fb_draw_circle(tray_x - 40, ty + TASKBAR_HEIGHT / 2, 5, 0x00CCCCCC);
}

static const char* window_type_name(window_t* win) {
    if (!win) return "?";
    if (win->type == WINDOW_TYPE_GSHELL) return "终端";
    if (win->type == WINDOW_TYPE_DEMO) return "演示";
    if (win->type == WINDOW_TYPE_ABOUT) return "关于";
    if (win->type == WINDOW_TYPE_CALCULATOR) return "计算器";
    if (win->type == WINDOW_TYPE_FILE_MANAGER) return "文件";
    if (win->type == WINDOW_TYPE_SETTINGS) return "设置";
    return "窗口";
}

void render_thumbnail(void) {
    if (taskbar_hover_index < 0 || taskbar_hover_index >= window_count) return;
    if ((int)timer_ticks - taskbar_hover_ticks < 25) return;

    window_t* win = &windows[taskbar_hover_index];
    if (!win->visible || win->minimized) return;

    int sw = (int)fb_width;
    int sh = (int)fb_height;
    int thumb_w = 160;
    int thumb_h = 120;
    int lx = taskbar_button_x(taskbar_hover_index);
    int tx = lx + TASKBAR_BTN_W / 2 - thumb_w / 2;
    int ty = sh - TASKBAR_HEIGHT - thumb_h - 8;
    if (tx < 8) tx = 8;
    if (tx + thumb_w > sw - 8) tx = sw - thumb_w - 8;

    fb_draw_rect_outline(tx, ty, thumb_w, thumb_h, 0x00FFFFFF);
    fb_fillrect(tx + 1, ty + 1, thumb_w - 2, thumb_h - 2, 0x00333333);
    draw_string_centered(tx, ty + 48, thumb_w, window_type_name(win), COLOR_TEXT_WHITE);
}

void render_clock_bubble(void) {
    if (!taskbar_clock_hovered) return;
    int sw = (int)fb_width;
    int sh = (int)fb_height;
    int ty = sh - TASKBAR_HEIGHT;
    int time_w = string_width(time_buf);
    int bw_w = time_w + 16;
    int cx = sw - bw_w - 12;
    const char* date_str = "2026-07-09";
    int dw = string_width(date_str) + 24;
    int dh = 28;
    int bx = cx + bw_w / 2 - dw / 2;
    int by = ty - dh - 6;
    if (bx < 8) bx = 8;
    if (bx + dw > sw - 8) bx = sw - dw - 8;
    fb_fillrect_alpha(bx, by, dw, dh, 0xE8222222u);
    fb_draw_round_rect(bx, by, dw, dh, 4, 0x00555555);
    draw_string_centered_bold(bx, by + (dh - 16) / 2, dw, date_str, COLOR_TEXT_WHITE);
}
