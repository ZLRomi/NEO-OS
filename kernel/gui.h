#ifndef GUI_H
#define GUI_H

#include <stdint.h>

extern volatile int gui_active;
extern volatile int gui_start_requested;

void gui_run(void);
void gui_request_exit(void);

void gshell_putchar(char c);
void gshell_puts(const char* s);
void gshell_clear(void);
void gshell_input_char(char c);
void gshell_input_backspace(void);
void gshell_input_submit(void);

/* 开始菜单 */
#define START_MENU_ITEMS 9
#define START_MENU_ITEM_HEIGHT 28
#define START_MENU_MIN_WIDTH 160
#define START_MENU_PADDING_X 24

typedef struct {
    const char* label;
    void (*callback)(void);
} start_menu_item_t;

typedef struct {
    int visible;
    int x, y, w;
    int selected_item;
    start_menu_item_t items[START_MENU_ITEMS];
} start_menu_t;

void start_menu_init(void);
void start_menu_toggle(void);
void start_menu_hide(void);
int start_menu_is_visible(void);
void start_menu_handle_click(int mx, int my);
void start_menu_hover_update(int mx, int my);
void start_menu_update_animation(void);
int start_menu_is_animating(void);
int start_menu_get_panel_y(void);

/* 电源菜单 */
void power_menu_show(int x, int y);
void power_menu_hide(void);
int power_menu_is_visible(void);
void power_menu_handle_click(int mx, int my);
void power_menu_update_hover(int mx, int my);
void render_power_menu(void);

/* 窗口优先级系统 */
#define MAX_WINDOWS 11
#define WINDOW_TYPE_DEMO 1
#define WINDOW_TYPE_GSHELL 2
#define WINDOW_TYPE_ABOUT 3
#define WINDOW_TYPE_CALCULATOR 4
#define WINDOW_TYPE_BROWSER 5
#define WINDOW_TYPE_TASKMGR 6
#define WINDOW_TYPE_DOWNLOADS 7
#define WINDOW_TYPE_PLAYER 8
#define WINDOW_TYPE_FILE_MANAGER 9
#define WINDOW_TYPE_SETTINGS 10
#define WINDOW_TYPE_APP_STORE 11

typedef struct window {
    int type;
    int x, y, w, h;
    int visible;
    int minimized;
    int dragging;
    int drag_off_x, drag_off_y;
    int z_order;
    const char* title;
    void* data;
    /* normal bounds, preserved during minimize/restore */
    int normal_x, normal_y, normal_w, normal_h;
    /* animation */
    int animating;
    uint32_t anim_start;
    uint32_t anim_duration;
    int anim_sx, anim_sy, anim_sw, anim_sh;
    int anim_ex, anim_ey, anim_ew, anim_eh;
    int anim_hide_on_finish;
} window_t;

/* 计算器状态 */
typedef struct {
    int display;
    int operand;
    char op;
    int has_op;
    int entering;
} calculator_t;

/* GSHELL 状态 */
typedef struct {
    char output[4096];
    int output_len;
    char input[256];
    int input_len;
} gshell_t;

/* 浏览器状态 */
#define BROWSER_URL_SIZE     256
#define BROWSER_CONTENT_SIZE 65536
#define BROWSER_RAW_SIZE     65536
#define BROWSER_MAX_LINKS    32

typedef struct {
    int start;
    int end;
    char url[BROWSER_URL_SIZE];
} browser_link_t;

typedef struct {
    char url[BROWSER_URL_SIZE];
    char content[BROWSER_CONTENT_SIZE];
    char raw_html[BROWSER_RAW_SIZE];
    int content_len;
    int loading;
    int scroll_y;
    char status[64];
    char back_url[BROWSER_URL_SIZE];
    char forward_url[BROWSER_URL_SIZE];
    int url_focused;
    browser_link_t links[BROWSER_MAX_LINKS];
    int link_count;
} browser_t;

/* 右键菜单 */
#define CONTEXT_MENU_ITEMS 8

typedef struct {
    int visible;
    int x, y;
    int selected_item;
    int type;
    int item_count;
    int width;
    const char* labels[CONTEXT_MENU_ITEMS];
    int separator_before[CONTEXT_MENU_ITEMS];
} context_menu_t;

#define CONTEXT_MENU_DESKTOP 1
#define CONTEXT_MENU_TASKBAR 2

void window_bring_to_front(window_t* win);
window_t* window_get_at(int mx, int my);
window_t* window_find_by_type(int type);
void window_render_all(void);

/* GUI 键盘输入 */
extern volatile int gui_key_ready;
extern volatile char gui_key_char;
extern volatile uint8_t gui_key_scancode;
void gui_handle_key(char c, uint8_t sc);

/* GUI 状态 */
#define GUI_STATE_LOCKSCREEN 0
#define GUI_STATE_LOGIN      1
#define GUI_STATE_DESKTOP    2

extern volatile int gui_state;

/* 桌面图标 */
#define DESKTOP_ICON_COUNT 7
#define DESKTOP_ICON_SIZE  32

typedef struct {
    int type;
    const char* label;
    int x, y;
    int selected;
    int hovered;
} desktop_icon_t;

extern desktop_icon_t desktop_icons[DESKTOP_ICON_COUNT];

#endif
