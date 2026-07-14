#include "gui_internal.h"
#include "wallpaper.h"
#include "gfx_settings.h"
#include "fat32.h"
#include "memory.h"
#include "idt.h"
#include "io.h"
#include "serial.h"
#include "easing.h"
#include "theme.h"
#include "power.h"
#include <stdint.h>
#include <stddef.h>

start_menu_t start_menu;
context_menu_t context_menu;
context_menu_t taskbar_menu;

#define SM_PANEL_W   480
#define SM_PANEL_H   440
#define SM_RADIUS    8
#define SM_GRID_COLS 3
#define SM_GRID_ROWS 3
#define SM_CELL_W    140
#define SM_CELL_H    70
#define SM_GRID_X    16
#define SM_GRID_Y    64
#define SM_SEARCH_H  32

static int start_menu_anim_offset = 0;
static int start_menu_animating = 0;
static int start_menu_anim_show = 0;
static int start_menu_anim_id = 0;
static int start_menu_visible_internal = 0;

static int start_menu_panel_h(void) {
    return SM_PANEL_H;
}

static int start_menu_panel_x(void) {
    return (int)vbe_get_width() / 2 - SM_PANEL_W / 2;
}

static int start_menu_panel_base_y(void) {
    return (int)vbe_get_height() - TASKBAR_HEIGHT - SM_PANEL_H - 8;
}

static int start_menu_sorted_index(int slot) {
    static const int sorted[] = { 3, 5, 2, 4, 1, 0, 6, 7, 8 };
    if (slot < 0 || slot >= START_MENU_ITEMS) return -1;
    return sorted[slot];
}

static void start_menu_anim_done_show(int id) {
    (void)id;
    start_menu_animating = 0;
    start_menu_anim_id = 0;
    start_menu_anim_offset = 0;
    start_menu_visible_internal = 1;
    gui_dirty = 1;
}

static void start_menu_anim_done_hide(int id) {
    (void)id;
    start_menu_animating = 0;
    start_menu_anim_id = 0;
    start_menu_anim_offset = SM_PANEL_H;
    start_menu_visible_internal = 0;
    gui_dirty = 1;
}

#define POWER_MENU_ITEMS 3
static int power_menu_visible = 0;
static int power_menu_x = 0;
static int power_menu_y = 0;
static int power_menu_selected = -1;
static const char* power_menu_labels[POWER_MENU_ITEMS] = { "锁定", "重启", "关机" };

static void menu_item_terminal(void) { desktop_icon_open(WINDOW_TYPE_GSHELL); start_menu_hide(); }
static void menu_item_demo(void)     { desktop_icon_open(WINDOW_TYPE_DEMO);   start_menu_hide(); }
static void menu_item_about(void)    { desktop_icon_open(WINDOW_TYPE_ABOUT);  start_menu_hide(); }
static void menu_item_calc(void)     { desktop_icon_open(WINDOW_TYPE_CALCULATOR); start_menu_hide(); }
static void menu_item_clear(void)    { gshell_clear(); start_menu_hide(); }
static void menu_item_browser(void)  { desktop_icon_open(WINDOW_TYPE_BROWSER); start_menu_hide(); }
static void menu_item_files(void)    { desktop_icon_open(WINDOW_TYPE_FILE_MANAGER); start_menu_hide(); }
static void menu_item_settings(void) { desktop_icon_open(WINDOW_TYPE_SETTINGS); start_menu_hide(); }
static void menu_item_appstore(void) { app_store_open(); start_menu_hide(); }

static void context_menu_refresh(void) { gui_dirty = 1; context_menu.visible = 0; }

static void context_menu_wallpaper(void) {
    context_menu.visible = 0;
    static const char* candidates[] = {
        "/wallpaper.bmp", "/wallpaper.jpg", "/wallpaper.jpeg",
        "/etc/wallpaper.bmp", "/etc/wallpaper.jpg",
        "/wallpapers/wp.bmp", "/wallpapers/wp.jpg"
    };
    int n = (int)(sizeof(candidates) / sizeof(candidates[0]));
    int i;
    for (i = 0; i < n; i++) {
        if (wallpaper_load(candidates[i], g_wallpaper_mode) == 0) {
            wallpaper_save_config();
            gui_dirty = 1;
            return;
        }
    }
    serial_write("wallpaper: no candidate image found\n");
    gui_dirty = 1;
}

static void context_menu_display_settings(void) {
    context_menu.visible = 0;
    gfx_settings_open();
    gui_dirty = 1;
}

static void context_menu_new_folder(void) {
    context_menu.visible = 0;
    fat32_mkdir("/New Folder");
    gui_dirty = 1;
}

static void context_menu_properties(void) {
    context_menu.visible = 0;
    desktop_icon_open(WINDOW_TYPE_ABOUT);
}

void context_menu_init(void) {
    int i;
    context_menu.visible = 0;
    context_menu.x = 0;
    context_menu.y = 0;
    context_menu.selected_item = -1;
    context_menu.type = CONTEXT_MENU_DESKTOP;
    context_menu.width = 200;
    context_menu.item_count = 5;
    for (i = 0; i < CONTEXT_MENU_ITEMS; i++) {
        context_menu.labels[i] = NULL;
        context_menu.separator_before[i] = 0;
    }
    context_menu.labels[0] = "刷新";
    context_menu.labels[1] = "更换壁纸";
    context_menu.labels[2] = "显示设置";
    context_menu.labels[3] = "新建文件夹";
    context_menu.labels[4] = "属性";
    context_menu.separator_before[1] = 1;
    context_menu.separator_before[3] = 1;
}

void context_menu_show(int mx, int my) {
    context_menu.visible = 1;
    context_menu.x = mx;
    context_menu.y = my;
    context_menu.selected_item = -1;
}

void context_menu_hide(void) {
    context_menu.visible = 0;
    context_menu.selected_item = -1;
}

void context_menu_handle_click(int mx, int my) {
    if (!context_menu.visible) return;
    int item_h = START_MENU_ITEM_HEIGHT;
    int w = context_menu.width ? context_menu.width : 200;
    int h = context_menu.item_count * item_h;
    if (mx < context_menu.x || mx >= context_menu.x + w ||
        my < context_menu.y || my >= context_menu.y + h) {
        context_menu_hide();
        return;
    }
    int idx = (my - context_menu.y) / item_h;
    if (idx < 0 || idx >= context_menu.item_count) {
        context_menu_hide();
        return;
    }
    if (idx == 0) context_menu_refresh();
    else if (idx == 1) context_menu_wallpaper();
    else if (idx == 2) context_menu_display_settings();
    else if (idx == 3) context_menu_new_folder();
    else if (idx == 4) context_menu_properties();
}

void taskbar_menu_init(void) {
    int i;
    taskbar_menu.visible = 0;
    taskbar_menu.x = 0;
    taskbar_menu.y = 0;
    taskbar_menu.selected_item = -1;
    taskbar_menu.type = CONTEXT_MENU_TASKBAR;
    taskbar_menu.width = 160;
    taskbar_menu.item_count = 3;
    for (i = 0; i < CONTEXT_MENU_ITEMS; i++) {
        taskbar_menu.labels[i] = NULL;
        taskbar_menu.separator_before[i] = 0;
    }
    taskbar_menu.labels[0] = "工具栏";
    taskbar_menu.labels[1] = "层叠窗口";
    taskbar_menu.labels[2] = "显示桌面";
}

void taskbar_menu_show(int mx, int my) {
    taskbar_menu.visible = 1;
    taskbar_menu.x = mx;
    taskbar_menu.y = my;
    taskbar_menu.selected_item = -1;
}

void taskbar_menu_hide(void) {
    taskbar_menu.visible = 0;
    taskbar_menu.selected_item = -1;
}

void taskbar_menu_handle_click(int mx, int my) {
    if (!taskbar_menu.visible) return;
    int item_h = START_MENU_ITEM_HEIGHT;
    int w = taskbar_menu.width ? taskbar_menu.width : 160;
    int h = taskbar_menu.item_count * item_h;
    if (mx < taskbar_menu.x || mx >= taskbar_menu.x + w ||
        my < taskbar_menu.y || my >= taskbar_menu.y + h) {
        taskbar_menu_hide();
        return;
    }
    int idx = (my - taskbar_menu.y) / item_h;
    if (idx < 0 || idx >= taskbar_menu.item_count) {
        taskbar_menu_hide();
        return;
    }
    if (idx == 2) {
        for (int i = 0; i < window_count; i++) {
            if (windows[i].type != WINDOW_TYPE_GSHELL) windows[i].minimized = 1;
        }
    }
    taskbar_menu_hide();
}

void power_menu_show(int x, int y) {
    power_menu_visible = 1;
    power_menu_x = x;
    power_menu_y = y;
    power_menu_selected = -1;
}

void power_menu_hide(void) {
    power_menu_visible = 0;
    power_menu_selected = -1;
}

int power_menu_is_visible(void) {
    return power_menu_visible;
}

void power_menu_handle_click(int mx, int my) {
    if (!power_menu_visible) return;
    int item_h = START_MENU_ITEM_HEIGHT;
    int w = 160;
    int h = POWER_MENU_ITEMS * item_h;
    if (mx < power_menu_x || mx >= power_menu_x + w ||
        my < power_menu_y || my >= power_menu_y + h) {
        power_menu_hide();
        return;
    }
    int idx = (my - power_menu_y) / item_h;
    if (idx == 0) {
        extern volatile int gui_state;
        gui_state = GUI_STATE_LOCKSCREEN;
        power_menu_hide();
        start_menu_hide();
    } else if (idx == 1) {
        power_reboot();
        while (1) __asm__ volatile("hlt");
    } else if (idx == 2) {
        power_off();
        while (1) __asm__ volatile("hlt");
    }
}

void power_menu_update_hover(int mx, int my) {
    if (!power_menu_visible) return;
    int item_h = START_MENU_ITEM_HEIGHT;
    int w = 160;
    int h = POWER_MENU_ITEMS * item_h;
    if (mx < power_menu_x || mx >= power_menu_x + w ||
        my < power_menu_y || my >= power_menu_y + h) {
        power_menu_selected = -1;
        return;
    }
    power_menu_selected = (my - power_menu_y) / item_h;
}

void render_power_menu(void) {
    if (!power_menu_visible) return;
    int item_h = START_MENU_ITEM_HEIGHT;
    int w = 160;
    int h = POWER_MENU_ITEMS * item_h;
    fb_fillrect(power_menu_x, power_menu_y, w, h, COLOR_MENU_BG);
    fb_draw_rect_outline(power_menu_x, power_menu_y, w, h, COLOR_MENU_BORDER);
    for (int i = 0; i < POWER_MENU_ITEMS; i++) {
        int item_y = power_menu_y + i * item_h;
        uint32_t bg = (i == power_menu_selected) ? COLOR_MENU_HOVER : COLOR_MENU_BG;
        fb_fillrect(power_menu_x + 2, item_y + 2, w - 4, item_h - 4, bg);
        draw_string(power_menu_x + 12, item_y + 6, power_menu_labels[i], COLOR_MENU_TEXT);
    }
}

void start_menu_init(void) {
    start_menu.visible = 0;
    start_menu.x = start_menu_panel_x();
    start_menu.y = start_menu_panel_base_y();
    start_menu.w = SM_PANEL_W;
    start_menu.selected_item = -1;
    start_menu_anim_offset = SM_PANEL_H;
    start_menu_animating = 0;
    start_menu_anim_id = 0;
    start_menu_visible_internal = 0;

    start_menu.items[0].label = "终端";     start_menu.items[0].callback = menu_item_terminal;
    start_menu.items[1].label = "演示窗口";  start_menu.items[1].callback = menu_item_demo;
    start_menu.items[2].label = "计算器";   start_menu.items[2].callback = menu_item_calc;
    start_menu.items[3].label = "关于";        start_menu.items[3].callback = menu_item_about;
    start_menu.items[4].label = "清空终端"; start_menu.items[4].callback = menu_item_clear;
    start_menu.items[5].label = "浏览器";      start_menu.items[5].callback = menu_item_browser;
    start_menu.items[6].label = "文件管理器"; start_menu.items[6].callback = menu_item_files;
    start_menu.items[7].label = "设置";    start_menu.items[7].callback = menu_item_settings;
    start_menu.items[8].label = "应用商店";   start_menu.items[8].callback = menu_item_appstore;
}

void start_menu_toggle(void) {
    if (start_menu_anim_id) {
        anim_stop(start_menu_anim_id);
        start_menu_anim_id = 0;
    }
    start_menu_animating = 1;
    int ph = start_menu_panel_h();
    if (start_menu_visible_internal) {
        start_menu_anim_show = 0;
        start_menu_anim_id = anim_start(&start_menu_anim_offset, 200, 0, ph, start_menu_anim_done_hide);
    } else {
        start_menu_anim_show = 1;
        start_menu_visible_internal = 1;
        start_menu_anim_id = anim_start(&start_menu_anim_offset, 200, ph, 0, start_menu_anim_done_show);
    }
    start_menu.selected_item = -1;
}

void start_menu_hide(void) {
    if (start_menu_visible_internal && !start_menu_animating) {
        start_menu_anim_show = 0;
        start_menu_animating = 1;
        int ph = start_menu_panel_h();
        start_menu_anim_id = anim_start(&start_menu_anim_offset, 200, 0, ph, start_menu_anim_done_hide);
    }
    start_menu.selected_item = -1;
}

int start_menu_is_visible(void) {
    return start_menu_visible_internal || start_menu_animating;
}

void start_menu_update_animation(void) {
    (void)0;
}

int start_menu_is_animating(void) {
    return start_menu_animating;
}

int start_menu_get_panel_y(void) {
    return start_menu_panel_base_y() + start_menu_anim_offset;
}

static int sm_grid_cell_x(int col) {
    return start_menu_panel_x() + SM_GRID_X + col * SM_CELL_W;
}

static int sm_grid_cell_y(int row) {
    return start_menu_get_panel_y() + SM_GRID_Y + row * SM_CELL_H;
}

static int sm_item_at(int mx, int my) {
    int py = start_menu_get_panel_y();
    if (mx < start_menu_panel_x() || mx >= start_menu_panel_x() + SM_PANEL_W) return -1;
    if (my < py + SM_GRID_Y || my >= py + SM_GRID_Y + SM_GRID_ROWS * SM_CELL_H) return -1;
    int col = (mx - (start_menu_panel_x() + SM_GRID_X)) / SM_CELL_W;
    int row = (my - (py + SM_GRID_Y)) / SM_CELL_H;
    if (col < 0 || col >= SM_GRID_COLS || row < 0 || row >= SM_GRID_ROWS) return -1;
    int slot = row * SM_GRID_COLS + col;
    if (slot < 0 || slot >= START_MENU_ITEMS) return -1;
    return slot;
}

void start_menu_hover_update(int mx, int my) {
    if (!start_menu_visible_internal) return;
    int slot = sm_item_at(mx, my);
    int idx = (slot >= 0) ? start_menu_sorted_index(slot) : -1;
    if (idx != start_menu.selected_item) {
        start_menu.selected_item = idx;
        gui_dirty = 1;
    }
}

void start_menu_handle_click(int mx, int my) {
    if (!start_menu_visible_internal && !start_menu_animating) return;
    int px = start_menu_panel_x();
    int py = start_menu_get_panel_y();

    int power_x = px + SM_PANEL_W - 36;
    int power_y = py + SM_PANEL_H - 30;
    if (mx >= power_x && mx < power_x + 20 && my >= power_y && my < power_y + 20) {
        power_menu_show(px + SM_PANEL_W - 160, py + SM_PANEL_H - POWER_MENU_ITEMS * START_MENU_ITEM_HEIGHT - 8);
        return;
    }

    if (mx < px || mx >= px + SM_PANEL_W || my < py || my >= py + SM_PANEL_H) {
        start_menu_hide();
        return;
    }
    int slot = sm_item_at(mx, my);
    if (slot >= 0) {
        int idx = start_menu_sorted_index(slot);
        if (idx >= 0 && idx < START_MENU_ITEMS) {
            if (start_menu.items[idx].callback) {
                start_menu.items[idx].callback();
            }
        }
    }
}

static const char* sm_short_label(int idx) {
    static const char* labels[] = { "终端", "演示", "计算", "关于", "清空", "网页", "文件", "设置", "商店" };
    if (idx < 0 || idx >= START_MENU_ITEMS) return "?";
    return labels[idx];
}

static void draw_sm_grid_cell(int slot, int panel_x, int panel_y) {
    (void)panel_x;
    (void)panel_y;
    int col = slot % SM_GRID_COLS;
    int row = slot / SM_GRID_COLS;
    int cx = sm_grid_cell_x(col);
    int cy = sm_grid_cell_y(row);
    int idx = start_menu_sorted_index(slot);
    if (idx < 0) return;
    int is_selected = (idx == start_menu.selected_item);
    if (is_selected) {
        blend_round_rect(cx + 6, cy + 4, SM_CELL_W - 12, SM_CELL_H - 8, 6, 0x33A6C9FFu);
    }
    int icon_x = cx + (SM_CELL_W - 16) / 2;
    int icon_y = cy + 14;
    if (idx == 0) draw_icon_terminal_small(icon_x, icon_y);
    else if (idx == 1) draw_icon_demo_small(icon_x, icon_y);
    else if (idx == 2) draw_icon_calc_small(icon_x, icon_y);
    else if (idx == 3) draw_icon_about_small(icon_x, icon_y);
    else if (idx == 4) draw_icon_folder_small(icon_x, icon_y);
    else if (idx == 5) draw_icon_browser_small(icon_x, icon_y);
    else if (idx == 6) {
        fb_fillrect(icon_x, icon_y, 16, 16, 0x006699CC);
        fb_draw_rect_outline(icon_x, icon_y, 16, 16, 0x00444444);
        draw_icon_folder_small(icon_x, icon_y);
    } else if (idx == 7) {
        draw_icon_settings(icon_x, icon_y);
    } else if (idx == 8) {
        fb_fillrect(icon_x, icon_y, 16, 16, g_theme.accent);
        fb_draw_rect_outline(icon_x, icon_y, 16, 16, 0x00444444);
    }
    draw_string_centered(cx, cy + 36, SM_CELL_W, sm_short_label(idx), COLOR_MENU_TEXT);
}

void render_start_menu_new(void) {
    if (!start_menu_visible_internal && !start_menu_animating) return;

    int px = start_menu_panel_x();
    int py = start_menu_get_panel_y();

    int clip_to_y = (int)vbe_get_height() - TASKBAR_HEIGHT;
    fb_set_clip(0, 0, (int)vbe_get_width(), clip_to_y);

    fb_box_blur(px, py, SM_PANEL_W, SM_PANEL_H, 4);
    blend_round_rect(px, py, SM_PANEL_W, SM_PANEL_H, SM_RADIUS, 0xE6F3F3F3u);
    fb_draw_round_rect(px, py, SM_PANEL_W, SM_PANEL_H, SM_RADIUS, 0x00CCCCCC);

    int sx = px + 16;
    int sy = py + 12;
    blend_round_rect(sx, sy, SM_PANEL_W - 32, SM_SEARCH_H, 6, 0xFFFFFFFFu);
    fb_draw_round_rect(sx, sy, SM_PANEL_W - 32, SM_SEARCH_H, 6, 0x00B0B0B0);
    draw_string(sx + 10, sy + (SM_SEARCH_H - 16) / 2, "搜索", 0x00888888);

    int gy = py + 48;
    draw_string_bold(px + 16, gy, "已固定", COLOR_MENU_TEXT);

    for (int slot = 0; slot < SM_GRID_COLS * SM_GRID_ROWS; slot++) {
        draw_sm_grid_cell(slot, px, py);
    }

    int ry = py + SM_GRID_Y + SM_GRID_ROWS * SM_CELL_H + 8;
    draw_string_bold(px + 16, ry, "最近使用", COLOR_MENU_TEXT);
    int rec_item_w = (SM_PANEL_W - 48) / 2;
    static const char* rec_labels[4] = { "文档1", "文档2", "阅读", "链接" };
    for (int i = 0; i < 4; i++) {
        int rx = px + 16 + (i % 2) * (rec_item_w + 8);
        int ryy = ry + 18 + (i / 2) * 36;
        blend_round_rect(rx, ryy, rec_item_w, 30, 4, 0x22FFFFFFu);
        draw_icon_folder_small(rx + 6, ryy + 7);
        draw_string(rx + 28, ryy + 8, rec_labels[i], COLOR_MENU_TEXT);
    }

    int fy = py + SM_PANEL_H - 36;
    fb_fillrect_alpha(px, fy, SM_PANEL_W, 36, 0x60EEEEEEu);
    draw_icon_user_small(px + 14, fy + 10);
    draw_string_bold(px + 36, fy + 10, "NEO", COLOR_MENU_TEXT);
    draw_icon_power(px + SM_PANEL_W - 32, fy + 10);

    fb_reset_clip();
}

void render_context_menu(context_menu_t* menu) {
    if (!menu->visible) return;
    int x = menu->x;
    int y = menu->y;
    int w = menu->width ? menu->width : 200;
    int item_h = START_MENU_ITEM_HEIGHT;
    int h = menu->item_count * item_h;
    int i;

    fb_fillrect(x, y, w, h, COLOR_MENU_BG);
    fb_draw_rect_outline(x, y, w, h, COLOR_MENU_BORDER);

    for (i = 0; i < menu->item_count; i++) {
        int item_y = y + i * item_h;
        if (menu->separator_before[i]) {
            fb_fillrect(x + 8, item_y, w - 16, 1, COLOR_MENU_BORDER);
        }
        uint32_t bg = (i == menu->selected_item) ? COLOR_MENU_HOVER : COLOR_MENU_BG;
        uint32_t fg = (i == menu->selected_item) ? COLOR_MENU_HOVER_TEXT : COLOR_MENU_TEXT;
        fb_fillrect(x + 2, item_y + 2, w - 4, item_h - 4, bg);
        int icon_x = x + 8;
        int icon_y = item_y + 6;
        if (menu->type == CONTEXT_MENU_DESKTOP) {
            if (i == 0) draw_icon_refresh(icon_x, icon_y);
            else if (i == 1) draw_icon_showdesktop(icon_x, icon_y);
            else if (i == 2) draw_icon_settings(icon_x, icon_y);
            else if (i == 3) draw_icon_folder_small(icon_x, icon_y);
            else if (i == 4) draw_icon_about_small(icon_x, icon_y);
        } else {
            if (i == 0) draw_icon_toolbar(icon_x, icon_y);
            else if (i == 1) draw_icon_cascade(icon_x, icon_y);
            else draw_icon_showdesktop(icon_x, icon_y);
        }
        if (menu->labels[i]) {
            draw_string(x + 30, item_y + 6, menu->labels[i], fg);
        }
    }
}
