#ifndef GUI_INTERNAL_H
#define GUI_INTERNAL_H

#include "gui.h"
#include "font_noto.h"
#include "css.h"
#include "download.h"
#include "player.h"
#include "file_manager.h"
#include "settings.h"
#include "app_store.h"
#include <stdint.h>

/* colors */
#define COLOR_DESKTOP         0x0078A6D6
#define COLOR_TASKBAR         0x00191919
#define COLOR_TASKBAR_HOVER   0x00333333
#define COLOR_WINDOW_TITLE    0x004466AA
#define COLOR_WINDOW_BODY     0x00E8E8E8
#define COLOR_CLOSE_BTN       0x00CC4444
#define COLOR_BTN_NORMAL      0x00555555
#define COLOR_BTN_HOVER       0x00666666
#define COLOR_TEXT_WHITE      0x00FFFFFF
#define COLOR_TEXT_BLACK      0x00000000
#define COLOR_TEXT_GRAY       0x00888888

#define COLOR_TERM_BG         0x00111111
#define COLOR_TERM_FG         0x00CCCCCC
#define COLOR_TERM_PROMPT     0x00FFFFFF

#define COLOR_MENU_BG         0x00F2F2F2
#define COLOR_MENU_BORDER     0x00D0D0D0
#define COLOR_MENU_HOVER      0x00D6EAF9
#define COLOR_MENU_HOVER_TEXT 0x00000000
#define COLOR_MENU_TEXT       0x00000000

#define COLOR_LOCK_BG_TOP     0x001E3A5F
#define COLOR_LOCK_BG_BOTTOM  0x002D5F8B
#define COLOR_LOGIN_PANEL     0x00E6E6E6
#define COLOR_LOGIN_INPUT     0x00FFFFFF
#define COLOR_LOGIN_INPUT_BD  0x00CCCCCC

#define COLOR_START_WIN_RED    0x00F25022
#define COLOR_START_WIN_GREEN  0x007FBA00
#define COLOR_START_WIN_BLUE   0x0000A4EF
#define COLOR_START_WIN_YELLOW 0x00FFB900

#define COLOR_ICON_BG          0x00FFFFFF
#define COLOR_ICON_TERM        0x00111111
#define COLOR_ICON_ABOUT       0x0000A4EF
#define COLOR_ICON_CALC        0x00707070
#define COLOR_ICON_DEMO        0x00FFB900

/* dimensions */
#define TASKBAR_HEIGHT 48
#define START_BTN_SIZE 36
#define DESKTOP_ICON_SPACING 16
#define DESKTOP_LABEL_H 16

/* framebuffer/backbuffer */
extern uint32_t* back_buffer;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

extern int clip_enabled;
extern int clip_x1, clip_y1, clip_x2, clip_y2;

#define DIRTY_RECT_MAX 8

typedef struct {
    int x, y, w, h;
} dirty_rect_t;

extern dirty_rect_t dirty_rects[DIRTY_RECT_MAX];
extern int dirty_count;

int fb_init(void);
void fb_free(void);
void fb_blit(void);
void fb_blit_dirty(void);
void mark_dirty(int x, int y, int w, int h);
void dirty_clear(void);
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip(void);
void fb_putpixel(int x, int y, uint32_t color);

static inline void fb_fillrect(int x, int y, int w, int h, uint32_t color) {
    if (!back_buffer) return;
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
    int width = x2 - x1;
    if (width <= 0 || y1 >= y2) return;
    for (int row = y1; row < y2; row++) {
        uint32_t* dst = back_buffer + row * pitch_words + x1;
        int n = width;
        while (n >= 4) {
            dst[0] = color;
            dst[1] = color;
            dst[2] = color;
            dst[3] = color;
            dst += 4;
            n -= 4;
        }
        while (n > 0) {
            *dst++ = color;
            n--;
        }
    }
}

void fb_fillrect_grad(int x, int y, int w, int h, uint32_t top, uint32_t bottom);
void fb_fillrect_vgrad(int x, int y, int w, int h, uint32_t color_top, uint32_t color_bottom);
void fb_fillrect_alpha(int x, int y, int w, int h, uint32_t color);
void fb_draw_rect_outline(int x, int y, int w, int h, uint32_t color);
void fb_draw_line(int x1, int y1, int x2, int y2, uint32_t color);
void fb_draw_circle(int cx, int cy, int r, uint32_t color);
void fb_fill_circle(int cx, int cy, int r, uint32_t color);
void fb_draw_round_rect(int x, int y, int w, int h, int r, uint32_t color);
void fb_draw_round_rect_top(int x, int y, int w, int h, int r, uint32_t color);
void fb_fillrect_vgrad_round_top(int x, int y, int w, int h, int r,
                                  uint32_t color_top, uint32_t color_bottom);
void fb_box_blur(int x, int y, int w, int h, int radius);
void fb_draw_logo(int cx, int cy, int scale);
void fb_show_boot_logo(void);
void image_blit(int x, int y, const uint32_t* data, int w, int h);
void blend_round_rect(int x, int y, int w, int h, int r, uint32_t color);

/* icons */
void draw_icon_terminal(int x, int y);
void draw_icon_terminal_small(int x, int y);
void draw_icon_about(int x, int y);
void draw_icon_about_small(int x, int y);
void draw_icon_calc(int x, int y);
void draw_icon_calc_small(int x, int y);
void draw_icon_demo(int x, int y);
void draw_icon_demo_small(int x, int y);
void draw_icon_user(int x, int y);
void draw_icon_user_small(int x, int y);
void draw_icon_power(int x, int y);
void draw_icon_folder(int x, int y);
void draw_icon_folder_small(int x, int y);
void draw_icon_settings(int x, int y);
void draw_icon_weather(int x, int y);
void draw_icon_news(int x, int y);
void draw_icon_mail(int x, int y);
void draw_icon_photos(int x, int y);
void draw_icon_refresh(int x, int y);
void draw_icon_toolbar(int x, int y);
void draw_icon_cascade(int x, int y);
void draw_icon_showdesktop(int x, int y);
void draw_icon_browser(int x, int y);
void draw_icon_browser_small(int x, int y);

/* drawing / text */
void draw_char(int x, int y, char c, uint32_t color);
void draw_char_2x(int x, int y, char c, uint32_t color);
void draw_char_bold(int x, int y, char c, uint32_t color);
void draw_char_cn(int x, int y, uint16_t code, uint32_t color);
void draw_char_cn_bold(int x, int y, uint16_t code, uint32_t color);
void draw_string(int x, int y, const char* s, uint32_t color);
void draw_string_bold(int x, int y, const char* s, uint32_t color);
void draw_string_centered(int x, int y, int w, const char* s, uint32_t color);
void draw_string_centered_bold(int x, int y, int w, const char* s, uint32_t color);
void draw_string_wrapped(int x, int y, int max_w, int max_h, const char* s, uint32_t color);
int string_width(const char* s);
int utf8_char_count(const char* s);
void int_to_str(int v, char* out);
void format_time(char* buf, int ticks);
void draw_cursor(int x, int y);

#define CURSOR_W 11
#define CURSOR_H 16
extern const uint16_t cursor_outline[16];
extern const uint16_t cursor_fill[16];
void cursor_draw_front(int x, int y);
void cursor_erase_front(int x, int y, int w, int h);
void cursor_save_bg(int x, int y);
void cursor_restore_bg(void);

/* dirty flag */
extern volatile int gui_dirty;

/* desktop icons */
void init_desktop_icons(void);
void render_desktop_icons(int mx, int my);
void desktop_icon_handle_click(int mx, int my);
void desktop_icon_open(int type);

/* lockscreen / login */
void render_lockscreen(void);
void render_login(void);
void login_handle_key(char c, uint8_t sc);
extern int login_anim_active;

/* menus */
extern start_menu_t start_menu;
extern context_menu_t context_menu;
extern context_menu_t taskbar_menu;

void render_start_menu_new(void);
void context_menu_init(void);
void context_menu_show(int mx, int my);
void context_menu_hide(void);
void context_menu_handle_click(int mx, int my);
void taskbar_menu_init(void);
void taskbar_menu_show(int mx, int my);
void taskbar_menu_hide(void);
void taskbar_menu_handle_click(int mx, int my);
void render_context_menu(context_menu_t* menu);

/* windows */
extern window_t windows[MAX_WINDOWS];
extern int window_count;
extern int next_z_order;

void window_bring_to_front_internal(window_t* win);
window_t* window_get_at(int mx, int my);
window_t* window_top_visible(void);
void init_window_system(void);
void clamp_window(window_t* win);
void window_render_all(void);
void window_update_animations(void);
void window_start_animation(window_t* win, uint32_t duration,
                            int sx, int sy, int sw, int sh,
                            int ex, int ey, int ew, int eh,
                            int hide_on_finish);

/* apps / window contents */
extern gshell_t gshell;
extern calculator_t calc_state;
extern browser_t browser;

void gshell_puts(const char* s);
void render_title_bar(window_t* win, uint32_t title_color);
void render_gshell(window_t* win);
void render_demo_window(window_t* win);
void render_about_window(window_t* win);
void render_calculator(window_t* win);
void calculator_handle_click(window_t* win, int mx, int my);

/* task manager */
void render_taskmgr(window_t* win);
void taskmgr_handle_click(window_t* win, int mx, int my);

/* downloads */
void render_downloads(window_t* win);
void downloads_handle_click(window_t* win, int mx, int my);

/* player */
void render_player(window_t* win);
void player_handle_click(window_t* win, int mx, int my);

/* file manager */
void file_manager_handle_key(char c, uint8_t sc);

/* browser */
void render_browser(window_t* win);
void browser_handle_key(char c, uint8_t sc);
void browser_handle_click(window_t* win, int mx, int my);
void browser_navigate(const char* url);
void browser_init(void);
extern css_engine_t browser_engine;

/* taskbar */
#define TASKBAR_BTN_W   44
#define TASKBAR_BTN_H   40
#define TASKBAR_BTN_GAP 4
void render_taskbar(int mx, int my);
void render_thumbnail(void);
void render_clock_bubble(void);
int  taskbar_start_btn_x(void);
int  taskbar_start_btn_y(void);
int  taskbar_list_start(void);
int  taskbar_list_y(void);
int  taskbar_tray_x(void);
int  taskbar_clock_hit(int mx, int my);
int  taskbar_clock_hover(void);
int  taskbar_start_btn_hit(int mx, int my);
int  taskbar_button_at(int mx, int my);
int  taskbar_button_x(int idx);
int  taskbar_button_y(void);

/* main rendering */
void render_desktop(void);
void render_gui(void);
void update_desktop(int mx, int my, uint8_t buttons);

#endif
