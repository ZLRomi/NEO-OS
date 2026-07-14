#include "gui_internal.h"
#include "memory.h"
#include "idt.h"
#include "cmd.h"
#include "http.h"
#include "net.h"
#include "printk.h"
#include "serial.h"
#include "css.h"
#include "mouse.h"
#include "theme.h"
#include <stdint.h>
#include <stddef.h>

#define GSHELL_BUF_SIZE 4096
#define GSHELL_INPUT_SIZE 256

gshell_t gshell = { {0}, 0, {0}, 0 };
calculator_t calc_state = { 0, 0, 0, 0, 1 };
browser_t browser = { 0 };

#define BROWSER_MAX_TABS 4
static struct {
    char url[BROWSER_MAX_TABS][BROWSER_URL_SIZE];
    int active;
    int count;
} browser_tabs = { {{0}}, 0, 1 };
static char browser_hover_url[BROWSER_URL_SIZE] = {0};

void gshell_clear(void) {
    gshell.output_len = 0;
    gshell.output[0] = '\0';
    gui_dirty = 1;
}

void gshell_putchar(char c) {
    gui_dirty = 1;
    if (c == '\b') {
        if (gshell.output_len > 0) gshell.output_len--;
        return;
    }
    if (gshell.output_len >= GSHELL_BUF_SIZE - 2) {
        int half = GSHELL_BUF_SIZE / 2;
        for (int i = 0; i < half; i++) gshell.output[i] = gshell.output[i + half];
        gshell.output_len = half;
    }
    gshell.output[gshell.output_len++] = c;
    gshell.output[gshell.output_len] = '\0';
}

void gshell_input_char(char c) {
    window_t* win = window_find_by_type(WINDOW_TYPE_GSHELL);
    int max_visible = win ? (win->w - 16 - FONT_WIDTH * 2) / FONT_WIDTH : 40;
    if (max_visible < 0) max_visible = 0;
    if (gshell.input_len < GSHELL_INPUT_SIZE - 1 && gshell.input_len < max_visible) {
        gshell.input[gshell.input_len++] = c;
        gshell.input[gshell.input_len] = '\0';
        gui_dirty = 1;
    }
}

void gshell_input_backspace(void) {
    if (gshell.input_len > 0) {
        gshell.input_len--;
        gshell.input[gshell.input_len] = '\0';
        gui_dirty = 1;
    }
}

void gshell_puts(const char* s) {
    while (*s) gshell_putchar(*s++);
}

void gshell_input_submit(void) {
    gshell_putchar('\n');
    gshell_puts("GSHELL> ");
    for (int i = 0; i < gshell.input_len; i++) {
        gshell_putchar(gshell.input[i]);
    }
    gshell_putchar('\n');

    char cmd[GSHELL_INPUT_SIZE];
    for (int i = 0; i <= gshell.input_len; i++) cmd[i] = gshell.input[i];
    gshell.input_len = 0;
    gshell.input[0] = '\0';

    cmd_parse(cmd);
    gshell_puts("GSHELL> ");
}

void render_title_bar(window_t* win, uint32_t title_color) {
    int x = win->x, y = win->y, w = win->w;
    int tb_h = 32;
    int r = g_theme.corner_radius;
    if (r < 0) r = 0;
    if (r > 8) r = 8;
    int tr = (title_color >> 16) & 0xFF;
    int tg = (title_color >> 8) & 0xFF;
    int tb = title_color & 0xFF;
    uint32_t bottom_color = ((uint32_t)(tr * 4 / 5) << 16) |
                            ((uint32_t)(tg * 4 / 5) << 8) |
                            (uint32_t)(tb * 4 / 5);
    fb_fillrect_vgrad_round_top(x, y, w, tb_h, r, title_color, bottom_color);
    draw_string_bold(x + 12, y + 4, win->title ? win->title : "窗口", COLOR_TEXT_WHITE);

    int btn_w = 28;
    int close_x = x + w - btn_w;
    int restore_x = close_x - btn_w;
    int minimize_x = restore_x - btn_w;
    int mx = mouse_x, my = mouse_y;

    if (mx >= minimize_x && mx < minimize_x + btn_w && my >= y && my < y + tb_h) {
        fb_fillrect_alpha(minimize_x, y, btn_w, tb_h, 0x20CCCCCC);
    }
    int mcx = minimize_x + btn_w / 2, mcy = y + tb_h / 2;
    fb_draw_line(mcx - 5, mcy + 5, mcx + 5, mcy + 5, COLOR_TEXT_WHITE);

    if (mx >= restore_x && mx < restore_x + btn_w && my >= y && my < y + tb_h) {
        fb_fillrect_alpha(restore_x, y, btn_w, tb_h, 0x20CCCCCC);
    }
    int rcx = restore_x + btn_w / 2, rcy = y + tb_h / 2;
    fb_draw_rect_outline(rcx - 5, rcy - 5, 10, 10, COLOR_TEXT_WHITE);

    if (mx >= close_x && mx < close_x + btn_w && my >= y && my < y + tb_h) {
        fb_fillrect_alpha(close_x, y, btn_w, tb_h, 0xC0C42B1C);
    }
    int ccx = close_x + btn_w / 2, ccy = y + tb_h / 2;
    fb_draw_line(ccx - 5, ccy - 5, ccx + 5, ccy + 5, COLOR_TEXT_WHITE);
    fb_draw_line(ccx + 5, ccy - 5, ccx - 5, ccy + 5, COLOR_TEXT_WHITE);
}

static int gshell_count_lines(int cols) {
    if (gshell.output_len == 0) return 0;
    int lines = 1;
    int cx = 0;
    for (int i = 0; i < gshell.output_len; i++) {
        char c = gshell.output[i];
        if (c == '\n') {
            lines++;
            cx = 0;
        } else {
            cx++;
            if (cx > cols) {
                lines++;
                cx = 1;
            }
        }
    }
    return lines;
}

static int gshell_find_start(int cols, int max_lines) {
    int total = gshell_count_lines(cols);
    if (total <= max_lines) return 0;
    int skip = total - max_lines;
    int lines = 1;
    int cx = 0;
    for (int i = 0; i < gshell.output_len; i++) {
        if (lines > skip) return i;
        char c = gshell.output[i];
        if (c == '\n') {
            lines++;
            cx = 0;
        } else {
            cx++;
            if (cx > cols) {
                lines++;
                cx = 1;
            }
        }
    }
    return 0;
}

void render_gshell(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, COLOR_TERM_BG);
    fb_fillrect_vgrad_round_top(x, y, w, 32, g_theme.corner_radius > 8 ? 8 : g_theme.corner_radius, 0x00222222, 0x00111111);
    draw_string_bold(x + 12, y + 4, win->title ? win->title : "终端", COLOR_TEXT_WHITE);

    int btn_w = 28;
    int close_x = x + w - btn_w;
    int restore_x = close_x - btn_w;
    int minimize_x = restore_x - btn_w;
    int mx = mouse_x, my = mouse_y;
    if (mx >= minimize_x && mx < minimize_x + btn_w && my >= y && my < y + 32) {
        fb_fillrect_alpha(minimize_x, y, btn_w, 32, 0x20CCCCCC);
    }
    int mcx = minimize_x + btn_w / 2, mcy = y + 16;
    fb_draw_line(mcx - 5, mcy + 5, mcx + 5, mcy + 5, COLOR_TEXT_WHITE);
    if (mx >= restore_x && mx < restore_x + btn_w && my >= y && my < y + 32) {
        fb_fillrect_alpha(restore_x, y, btn_w, 32, 0x20CCCCCC);
    }
    int rcx = restore_x + btn_w / 2, rcy = y + 16;
    fb_draw_rect_outline(rcx - 5, rcy - 5, 10, 10, COLOR_TEXT_WHITE);
    if (mx >= close_x && mx < close_x + btn_w && my >= y && my < y + 32) {
        fb_fillrect_alpha(close_x, y, btn_w, 32, 0xC0C42B1C);
    }
    int ccx = close_x + btn_w / 2, ccy = y + 16;
    fb_draw_line(ccx - 5, ccy - 5, ccx + 5, ccy + 5, COLOR_TEXT_WHITE);
    fb_draw_line(ccx + 5, ccy - 5, ccx - 5, ccy + 5, COLOR_TEXT_WHITE);

    int pad = 8;
    int scroll_w = 8;
    int text_x = x + pad;
    int text_y = y + 40;
    int text_w = w - 2 * pad - scroll_w - 4;
    int text_h = h - 40 - pad;
    int cols = text_w / FONT_WIDTH;
    int total_rows = text_h / FONT_HEIGHT;
    int output_rows = total_rows - 1;

    int start = gshell_find_start(cols, output_rows);
    int cx = 0, cy = 0;
    fb_set_clip(text_x, text_y, text_w, text_h);
    for (int i = start; i < gshell.output_len && cy < output_rows; i++) {
        char c = gshell.output[i];
        if (c == '\n') {
            cx = 0;
            cy++;
        } else {
            if (cx >= cols) { cx = 0; cy++; }
            if (cy >= output_rows) break;
            draw_char(text_x + cx * FONT_WIDTH, text_y + cy * FONT_HEIGHT, c, COLOR_TERM_FG);
            cx++;
        }
    }
    fb_reset_clip();
    int visible_output_lines = cy + (cx > 0 ? 1 : 0);

    int input_y = text_y + visible_output_lines * FONT_HEIGHT;
    int input_bottom = y + h - FONT_HEIGHT - pad;
    if (input_y > input_bottom) input_y = input_bottom;

    draw_string(text_x, input_y, ">", g_theme.accent);
    draw_string(text_x + FONT_WIDTH, input_y, gshell.input, COLOR_TERM_FG);

    int cursor_x = text_x + FONT_WIDTH + gshell.input_len * FONT_WIDTH;
    if ((timer_ticks / 25) % 2 == 0) {
        fb_fillrect(cursor_x, input_y, 2, FONT_HEIGHT, COLOR_TERM_FG);
    }

    int total_lines = gshell_count_lines(cols);
    int scroll_x = x + w - pad - scroll_w;
    int scroll_track_y = text_y;
    int scroll_track_h = text_h;
    fb_fillrect(scroll_x, scroll_track_y, scroll_w, scroll_track_h, 0x22333333);
    if (total_lines > output_rows && output_rows > 0) {
        int thumb_h = scroll_track_h * output_rows / total_lines;
        if (thumb_h < 16) thumb_h = 16;
        int thumb_y = scroll_track_y + scroll_track_h - thumb_h;
        fb_fillrect(scroll_x, thumb_y, scroll_w, thumb_h, 0x66CCCCCC);
    }
}

void render_demo_window(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, 8, COLOR_WINDOW_BODY);
    render_title_bar(win, COLOR_WINDOW_TITLE);
    draw_string_wrapped(x + 12, y + 48, w - 24, h - 72,
        "NEO-OS GUI Demo\nDrag title bar\nClick desktop icon\nType in GShell",
        COLOR_TEXT_BLACK);
}

void render_about_window(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    char mem_buf[64];
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int cx = x + w / 2;
    int logo_y = y + 56;
    fb_fill_circle(cx, logo_y, 24, g_theme.accent);
    draw_string_centered_bold(cx - 24, logo_y - 8, 48, "N", COLOR_TEXT_WHITE);

    draw_string_centered_bold(x, y + 96, w, "NEO-OS", g_theme.text);
    draw_string_centered(x, y + 120, w, "版本 1.0", g_theme.text_secondary);
    draw_string_centered(x, y + 142, w, "构建 2025", g_theme.text_secondary);
    draw_string_centered(x, y + 164, w, "作者：NEO Team", g_theme.text_secondary);
    draw_string_centered(x, y + 186, w, "许可证：MIT", g_theme.text_secondary);

    int_to_str((int)memory_get_total_kb(), mem_buf);
    char info[64];
    int i = 0, j = 0;
    const char* prefix = "总内存：";
    while (prefix[i]) info[j++] = prefix[i++];
    i = 0;
    while (mem_buf[i]) info[j++] = mem_buf[i++];
    const char* suffix = " KB";
    i = 0;
    while (suffix[i]) info[j++] = suffix[i++];
    info[j] = '\0';
    draw_string_centered(x, y + 214, w, info, g_theme.text_secondary);
}

static void calculator_compute(void) {
    if (!calc_state.has_op || calc_state.op == 0) return;
    switch (calc_state.op) {
        case '+': calc_state.display = calc_state.operand + calc_state.display; break;
        case '-': calc_state.display = calc_state.operand - calc_state.display; break;
        case '*': calc_state.display = calc_state.operand * calc_state.display; break;
        case '/':
            if (calc_state.display != 0) calc_state.display = calc_state.operand / calc_state.display;
            else calc_state.display = 0;
            break;
    }
    calc_state.operand = 0;
    calc_state.op = 0;
}

void calculator_handle_click(window_t* win, int mx, int my) {
    int x = win->x, y = win->y, w = win->w;
    int btn_w = (w - 40) / 4;
    int btn_h = 32;
    int start_y = y + 84;
    int start_x = x + 16;
    int row, col;
    const char* labels[4][4] = {
        { "7", "8", "9", "/" },
        { "4", "5", "6", "*" },
        { "1", "2", "3", "-" },
        { "C", "0", "=", "+" }
    };
    if (mx < start_x || my < start_y) return;
    col = (mx - start_x) / (btn_w + 4);
    row = (my - start_y) / (btn_h + 8);
    if (col < 0 || col >= 4 || row < 0 || row >= 4) return;

    char c = labels[row][col][0];
    if (c >= '0' && c <= '9') {
        int digit = c - '0';
        if (calc_state.entering) {
            calc_state.display = calc_state.display * 10 + digit;
        } else {
            calc_state.display = digit;
            calc_state.entering = 1;
        }
    } else if (c == 'C') {
        calc_state.display = 0;
        calc_state.operand = 0;
        calc_state.op = 0;
        calc_state.has_op = 0;
        calc_state.entering = 1;
    } else if (c == '=') {
        if (calc_state.has_op) {
            calculator_compute();
            calc_state.has_op = 0;
            calc_state.entering = 0;
        }
    } else {
        if (calc_state.has_op) calculator_compute();
        calc_state.operand = calc_state.display;
        calc_state.op = c;
        calc_state.has_op = 1;
        calc_state.entering = 0;
    }
    gui_dirty = 1;
}

void render_calculator(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    int mx = mouse_x, my = mouse_y;
    char disp_buf[16];
    const char* labels[4][4] = {
        { "7", "8", "9", "/" },
        { "4", "5", "6", "*" },
        { "1", "2", "3", "-" },
        { "C", "0", "=", "+" }
    };
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);
    int_to_str(calc_state.display, disp_buf);
    fb_fillrect(x + 16, y + 44, w - 32, 32, COLOR_TEXT_WHITE);
    fb_draw_round_rect(x + 16, y + 44, w - 32, 32, 4, g_theme.border);
    draw_string(x + w - 24 - 8 * FONT_WIDTH, y + 48, disp_buf, COLOR_TEXT_BLACK);

    int btn_w = (w - 40) / 4;
    int btn_h = 32;
    int start_y = y + 84;
    int start_x = x + 16;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int bx = start_x + col * (btn_w + 4);
            int by = start_y + row * (btn_h + 8);
            char lab = labels[row][col][0];
            int is_op = (lab == '/' || lab == '*' || lab == '-' || lab == '+');
            int is_eq = (lab == '=');
            int is_c = (lab == 'C');
            int hover = (mx >= bx && mx < bx + btn_w && my >= by && my < by + btn_h);
            uint32_t btn_color;
            uint32_t txt_color = COLOR_TEXT_WHITE;
            if (is_eq) btn_color = g_theme.accent;
            else if (is_c) btn_color = 0x00CC4444;
            else if (is_op) btn_color = g_theme.accent_hover;
            else btn_color = g_theme.text_secondary;
            if (hover) {
                int r = (btn_color >> 16) & 0xFF;
                int g = (btn_color >> 8) & 0xFF;
                int b = btn_color & 0xFF;
                r = (r * 4 + 255) / 5;
                g = (g * 4 + 255) / 5;
                b = (b * 4 + 255) / 5;
                btn_color = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
            fb_fillrect(bx, by, btn_w, btn_h, btn_color);
            fb_draw_round_rect(bx, by, btn_w, btn_h, 4, g_theme.border);
            draw_string_bold(bx + (btn_w - FONT_WIDTH) / 2, by + 4, labels[row][col], txt_color);
        }
    }
}

/* ===== Browser ===== */

static int browser_url_len = 0;
css_engine_t browser_engine;

void browser_init(void) {
    browser.url[0] = '\0';
    browser.content[0] = '\0';
    browser.raw_html[0] = '\0';
    browser.content_len = 0;
    browser.loading = 0;
    browser.scroll_y = 0;
    browser.status[0] = '\0';
    browser.back_url[0] = '\0';
    browser.forward_url[0] = '\0';
    browser.url_focused = 1;
    browser_url_len = 0;
    browser.link_count = 0;
    css_engine_init(&browser_engine);
    browser_tabs.count = 1;
    browser_tabs.active = 0;
    for (int i = 0; i < BROWSER_MAX_TABS; i++) browser_tabs.url[i][0] = '\0';
    browser_hover_url[0] = '\0';
}

static void browser_tabs_sync_url(const char* url) {
    if (!url || !*url) return;
    int a = browser_tabs.active;
    int i = 0;
    while (url[i] && i < BROWSER_URL_SIZE - 1) {
        browser_tabs.url[a][i] = url[i];
        i++;
    }
    browser_tabs.url[a][i] = '\0';
}

static void draw_browser_icon_back(int cx, int cy, uint32_t color) {
    fb_draw_line(cx + 4, cy - 5, cx - 4, cy, color);
    fb_draw_line(cx - 4, cy, cx + 4, cy + 5, color);
    fb_draw_line(cx + 3, cy - 4, cx - 3, cy, color);
    fb_draw_line(cx - 3, cy, cx + 3, cy + 4, color);
}

static void draw_browser_icon_forward(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 4, cy - 5, cx + 4, cy, color);
    fb_draw_line(cx + 4, cy, cx - 4, cy + 5, color);
    fb_draw_line(cx - 3, cy - 4, cx + 3, cy, color);
    fb_draw_line(cx + 3, cy, cx - 3, cy + 4, color);
}

static void draw_browser_icon_refresh(int cx, int cy, uint32_t color) {
    fb_draw_line(cx, cy - 5, cx + 4, cy - 3, color);
    fb_draw_line(cx + 4, cy - 3, cx + 5, cy, color);
    fb_draw_line(cx + 5, cy, cx + 4, cy + 3, color);
    fb_draw_line(cx + 4, cy + 3, cx, cy + 5, color);
    fb_draw_line(cx, cy + 5, cx - 4, cy + 3, color);
    fb_draw_line(cx - 4, cy + 3, cx - 5, cy, color);
    fb_draw_line(cx, cy - 5, cx + 3, cy - 7, color);
    fb_draw_line(cx, cy - 5, cx + 3, cy - 3, color);
}

static void draw_browser_icon_home(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 6, cy, cx, cy - 6, color);
    fb_draw_line(cx, cy - 6, cx + 6, cy, color);
    fb_draw_line(cx - 5, cy, cx - 5, cy + 5, color);
    fb_draw_line(cx + 5, cy, cx + 5, cy + 5, color);
    fb_draw_line(cx - 5, cy + 5, cx + 5, cy + 5, color);
    fb_draw_line(cx - 1, cy + 1, cx - 1, cy + 5, color);
    fb_draw_line(cx + 1, cy + 1, cx + 1, cy + 5, color);
}

static void draw_browser_icon_plus(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 4, cy, cx + 4, cy, color);
    fb_draw_line(cx, cy - 4, cx, cy + 4, color);
}

static void draw_browser_icon_close(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 3, cy - 3, cx + 3, cy + 3, color);
    fb_draw_line(cx + 3, cy - 3, cx - 3, cy + 3, color);
}

static void browser_set_status(const char* s);
static void browser_load(const char* url, int push_history);
void browser_navigate(const char* url);
static void browser_go_back(void);
static void browser_go_forward(void);

static void browser_get_layout(window_t* win, int* bar_y, int* url_x, int* url_w,
                               int* back_x, int* fwd_x, int* refresh_x, int* home_x) {
    int x = win->x, y = win->y, w = win->w;
    int tab_y = y + 32;
    int tab_h = 26;
    *bar_y = tab_y + tab_h + 2;
    int btn_w = 28;
    int gap = 4;
    *back_x = x + 8;
    *fwd_x = *back_x + btn_w + gap;
    *refresh_x = *fwd_x + btn_w + gap;
    *home_x = *refresh_x + btn_w + gap;
    *url_x = *home_x + btn_w + 8;
    *url_w = x + w - 8 - *url_x;
}

static void browser_home(void) {
    browser_navigate("http://neo-os.local/index.html");
}

static void browser_tab_close(int idx) {
    if (idx < 0 || idx >= browser_tabs.count) return;
    for (int i = idx; i < browser_tabs.count - 1; i++) {
        int k = 0;
        while (browser_tabs.url[i + 1][k]) {
            browser_tabs.url[i][k] = browser_tabs.url[i + 1][k];
            k++;
        }
        browser_tabs.url[i][k] = '\0';
    }
    browser_tabs.count--;
    browser_tabs.url[browser_tabs.count][0] = '\0';
    if (browser_tabs.active >= browser_tabs.count) {
        browser_tabs.active = browser_tabs.count - 1;
    }
    if (browser_tabs.active < 0) browser_tabs.active = 0;
    if (browser_tabs.count <= 0) {
        browser_tabs.count = 1;
        browser_tabs.active = 0;
        browser_tabs.url[0][0] = '\0';
    }
    int a = browser_tabs.active;
    if (browser_tabs.url[a][0]) {
        browser_navigate(browser_tabs.url[a]);
    } else {
        browser.url[0] = '\0';
        browser.content[0] = '\0';
        browser.content_len = 0;
        browser_engine.active = 0;
        browser_set_status("新标签页");
        gui_dirty = 1;
    }
}

static void browser_tab_new(void) {
    if (browser_tabs.count >= BROWSER_MAX_TABS) return;
    browser_tabs.url[browser_tabs.count][0] = '\0';
    browser_tabs.active = browser_tabs.count;
    browser_tabs.count++;
    browser.url[0] = '\0';
    browser.content[0] = '\0';
    browser.content_len = 0;
    browser_engine.active = 0;
    browser.url_focused = 1;
    browser_url_len = 0;
    browser_set_status("新标签页");
    gui_dirty = 1;
}

static void browser_tab_select(int idx) {
    if (idx < 0 || idx >= browser_tabs.count) return;
    if (idx == browser_tabs.active) return;
    browser_tabs.active = idx;
    if (browser_tabs.url[idx][0]) {
        browser_load(browser_tabs.url[idx], 0);
    } else {
        browser.url[0] = '\0';
        browser.content[0] = '\0';
        browser.content_len = 0;
        browser_engine.active = 0;
        browser.url_focused = 1;
        browser_url_len = 0;
        browser_set_status("新标签页");
        gui_dirty = 1;
    }
}

static int browser_tag_is(const char* tag, const char* name) {
    int i = 0;
    while (name[i]) {
        char a = tag[i];
        char b = name[i];
        if (a == '\0') return 0;
        if (a != b && a != b - 32) return 0;
        i++;
    }
    return tag[i] == '\0' || tag[i] == ' ' || tag[i] == '>' || tag[i] == '/';
}

static int browser_attr_is(const char* s, const char* name) {
    int i = 0;
    while (name[i]) {
        char a = s[i];
        char b = name[i];
        if (a == '\0') return 0;
        if (a != b && a != b - 32) return 0;
        i++;
    }
    return s[i] == '=' || s[i] == ' ' || s[i] == '\0' || s[i] == '>' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r';
}

static void browser_extract_href(const char* tag, char* out, int max) {
    out[0] = '\0';
    int i = 0;
    while (tag[i] && tag[i] != '>' && tag[i] != ' ') i++;
    while (tag[i] && tag[i] != '>') {
        while (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r') i++;
        if (!tag[i] || tag[i] == '>') break;
        if (browser_attr_is(&tag[i], "href")) {
            i += 4;
            while (tag[i] == ' ' || tag[i] == '\t') i++;
            if (tag[i] == '=') i++;
            while (tag[i] == ' ' || tag[i] == '\t') i++;
            char quote = 0;
            if (tag[i] == '"' || tag[i] == '\'') { quote = tag[i]; i++; }
            int j = 0;
            while (tag[i] && tag[i] != '>' && j < max - 1) {
                if (quote && tag[i] == quote) break;
                if (!quote && (tag[i] == ' ' || tag[i] == '\t' || tag[i] == '\n' || tag[i] == '\r')) break;
                out[j++] = tag[i++];
            }
            out[j] = '\0';
            return;
        }
        while (tag[i] && tag[i] != ' ' && tag[i] != '\t' && tag[i] != '\n' && tag[i] != '\r' && tag[i] != '>') i++;
    }
}

static int browser_parse_html(const char* html, char* text, int text_max,
                              browser_link_t* links, int max_links) {
    int in_tag = 0;
    int in_script = 0;
    int in_style = 0;
    int in_head = 0;
    int in_svg = 0;
    int in_link = 0;
    int tag_i = 0;
    char tag[64];
    int p = 0;
    int prev_space = 1;
    int link_count = 0;

    for (int i = 0; html[i] && p < text_max - 1; i++) {
        char c = html[i];

        if (in_script) {
            if (c == '<' && browser_tag_is(html + i + 1, "/script")) {
                in_script = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }
        if (in_style) {
            if (c == '<' && browser_tag_is(html + i + 1, "/style")) {
                in_style = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }
        if (in_head) {
            if (c == '<' && browser_tag_is(html + i + 1, "/head")) {
                in_head = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }
        if (in_svg) {
            if (c == '<' && browser_tag_is(html + i + 1, "/svg")) {
                in_svg = 0;
                while (html[i] && html[i] != '>') i++;
            }
            continue;
        }

        if (c == '<') {
            in_tag = 1;
            tag_i = 0;
            tag[0] = '\0';
            continue;
        }
        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tag[tag_i] = '\0';

                if (browser_tag_is(tag, "head")) {
                    in_head = 1;
                    continue;
                }
                if (browser_tag_is(tag, "script")) {
                    in_script = 1;
                    continue;
                }
                if (browser_tag_is(tag, "style")) {
                    in_style = 1;
                    continue;
                }
                if (browser_tag_is(tag, "svg")) {
                    in_svg = 1;
                    continue;
                }
                if (browser_tag_is(tag, "br")) {
                    text[p++] = '\n';
                    prev_space = 1;
                    continue;
                }
                if (browser_tag_is(tag, "a")) {
                    if (link_count < max_links) {
                        browser_extract_href(tag, links[link_count].url, BROWSER_URL_SIZE);
                        if (links[link_count].url[0]) {
                            links[link_count].start = p;
                            links[link_count].end = p;
                            in_link = 1;
                        }
                    }
                    continue;
                }
                if (tag[0] == '/' && browser_tag_is(tag + 1, "a")) {
                    if (in_link && link_count < max_links) {
                        links[link_count].end = p;
                        if (links[link_count].start < links[link_count].end) {
                            link_count++;
                        }
                        in_link = 0;
                    }
                    continue;
                }
                if (browser_tag_is(tag, "h1")) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 1; }
                    prev_space = 1; continue;
                }
                if (browser_tag_is(tag, "h2")) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 2; }
                    prev_space = 1; continue;
                }
                if (browser_tag_is(tag, "h3") || browser_tag_is(tag, "h4") ||
                    browser_tag_is(tag, "h5") || browser_tag_is(tag, "h6")) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 3; }
                    prev_space = 1; continue;
                }
                if (browser_tag_is(tag, "p") || browser_tag_is(tag, "div") ||
                    browser_tag_is(tag, "tr") || browser_tag_is(tag, "header") ||
                    browser_tag_is(tag, "nav") || browser_tag_is(tag, "section") ||
                    browser_tag_is(tag, "main") || browser_tag_is(tag, "footer") ||
                    browser_tag_is(tag, "article") || browser_tag_is(tag, "aside") ||
                    browser_tag_is(tag, "figure") || browser_tag_is(tag, "figcaption") ||
                    browser_tag_is(tag, "ul") || browser_tag_is(tag, "ol") ||
                    browser_tag_is(tag, "table") || browser_tag_is(tag, "tbody") ||
                    browser_tag_is(tag, "thead") || browser_tag_is(tag, "form") ||
                    browser_tag_is(tag, "fieldset") || browser_tag_is(tag, "details") ||
                    browser_tag_is(tag, "address")) {
                    if (p > 0 && text[p - 1] != '\n') {
                        text[p++] = '\n';
                        if (p < text_max - 1 && text[p - 1] != '\n') text[p++] = '\n';
                    }
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 0x10; }
                    prev_space = 1;
                    continue;
                }
                if (browser_tag_is(tag, "b") || browser_tag_is(tag, "strong")) {
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 5; }
                    continue;
                }
                if (browser_tag_is(tag, "i") || browser_tag_is(tag, "em")) {
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 6; }
                    continue;
                }
                if (browser_tag_is(tag, "blockquote") || browser_tag_is(tag, "quote")) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 8; }
                    prev_space = 1; continue;
                }
                if (browser_tag_is(tag, "code") || browser_tag_is(tag, "pre")) {
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 7; }
                    continue;
                }
                if (browser_tag_is(tag, "li")) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 9; }
                    text[p++] = ' ';
                    text[p++] = ' ';
                    prev_space = 0;
                    continue;
                }
                if (tag[0] == '/' && (browser_tag_is(tag + 1, "p") || browser_tag_is(tag + 1, "div") ||
                    browser_tag_is(tag + 1, "h1") || browser_tag_is(tag + 1, "h2") ||
                    browser_tag_is(tag + 1, "h3") || browser_tag_is(tag + 1, "h4") ||
                    browser_tag_is(tag + 1, "h5") || browser_tag_is(tag + 1, "h6") ||
                    browser_tag_is(tag + 1, "tr") || browser_tag_is(tag + 1, "blockquote") ||
                    browser_tag_is(tag + 1, "header") || browser_tag_is(tag + 1, "nav") ||
                    browser_tag_is(tag + 1, "section") || browser_tag_is(tag + 1, "main") ||
                    browser_tag_is(tag + 1, "footer") || browser_tag_is(tag + 1, "article") ||
                    browser_tag_is(tag + 1, "aside") || browser_tag_is(tag + 1, "figure") ||
                    browser_tag_is(tag + 1, "figcaption") || browser_tag_is(tag + 1, "ul") ||
                    browser_tag_is(tag + 1, "ol") || browser_tag_is(tag + 1, "table") ||
                    browser_tag_is(tag + 1, "tbody") || browser_tag_is(tag + 1, "thead") ||
                    browser_tag_is(tag + 1, "form") || browser_tag_is(tag + 1, "fieldset") ||
                    browser_tag_is(tag + 1, "details") || browser_tag_is(tag + 1, "address") ||
                    browser_tag_is(tag + 1, "li"))) {
                    if (p > 0 && text[p - 1] != '\n') text[p++] = '\n';
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 0x10; }
                    prev_space = 1;
                    continue;
                }
                if (tag[0] == '/' && (browser_tag_is(tag + 1, "b") || browser_tag_is(tag + 1, "strong") ||
                    browser_tag_is(tag + 1, "i") || browser_tag_is(tag + 1, "em") ||
                    browser_tag_is(tag + 1, "code") || browser_tag_is(tag + 1, "pre"))) {
                    if (p < text_max - 2) { text[p++] = '\x01'; text[p++] = 0x10; }
                    continue;
                }
                continue;
            } else {
                if (tag_i < 63) {
                    tag[tag_i++] = c;
                    tag[tag_i] = '\0';
                }
            }
            continue;
        }

        if (c == '\r') continue;
        if (c == '\n' || c == ' ' || c == '\t') {
            if (!prev_space) {
                text[p++] = ' ';
                prev_space = 1;
            }
            continue;
        }
        text[p++] = c;
        prev_space = 0;
        if (in_link && link_count < max_links) {
            links[link_count].end = p;
        }
    }
    text[p] = '\0';
    return link_count;
}

static void browser_set_status(const char* s) {
    int i = 0;
    while (s[i] && i < 63) { browser.status[i] = s[i]; i++; }
    browser.status[i] = '\0';
}

static int browser_str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void browser_str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int browser_str_equal(const char* a, const char* b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == b[i];
}

static int browser_str_starts_with(const char* s, const char* pre) {
    int i = 0;
    while (pre[i]) {
        if (s[i] != pre[i]) return 0;
        i++;
    }
    return 1;
}

static void browser_load(const char* url, int push_history) {
    if (!url || !*url) return;

    char old_url[BROWSER_URL_SIZE];
    browser_str_copy(old_url, browser.url, BROWSER_URL_SIZE);
    browser_str_copy(browser.url, url, BROWSER_URL_SIZE);
    browser_url_len = browser_str_len(browser.url);
    browser_tabs_sync_url(browser.url);
    browser.loading = 1;
    browser.scroll_y = 0;
    browser.url_focused = 0;
    browser_set_status("Loading...");
    gui_dirty = 1;

    if (push_history && old_url[0] && !browser_str_equal(old_url, browser.url)) {
        browser_str_copy(browser.back_url, old_url, BROWSER_URL_SIZE);
        browser.forward_url[0] = '\0';
    }

    const char* host = browser.url;
    char host_buf[128];

    const char* p = browser.url;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' && p[4] == 's' && p[5] == ':') {
        p += 6;
        if (p[0] == '/' && p[1] == '/') p += 2;
    } else if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' && p[4] == ':') {
        p += 5;
        if (p[0] == '/' && p[1] == '/') p += 2;
    }

    int i = 0;
    while (p[i] && p[i] != '/' && i < 127) {
        host_buf[i] = p[i];
        i++;
    }
    host_buf[i] = '\0';
    host = host_buf;

    serial_write("BROWSER: host=");
    serial_write(host);
    serial_write(" dns_ip=");
    char ipb[16];
    net_ip_to_str(dns_ip, ipb);
    serial_write(ipb);
    serial_write("\n");

    browser.link_count = 0;
    int n = http_fetch(browser.url, browser.raw_html, BROWSER_RAW_SIZE, NULL, 0);
    if (n > 0) {
        browser.raw_html[n] = '\0';
        serial_write("BROWSER: raw first 200 bytes:\n");
        int dump_len = n > 200 ? 200 : n;
        extern void serial_write_hex(const uint8_t* data, int len);
        serial_write_hex((const uint8_t*)browser.raw_html, dump_len);
        serial_write("\n");
        browser.link_count = browser_parse_html(browser.raw_html, browser.content, BROWSER_CONTENT_SIZE,
                                                browser.links, BROWSER_MAX_LINKS);
        browser.content_len = browser_str_len(browser.content);

        css_engine_init(&browser_engine);
        int dom_nodes = dom_parse_html(&browser_engine, browser.raw_html);
        if (dom_nodes > 0) {
            int link_idx = 0;
            for (int ni = 0; ni < browser_engine.node_count && link_idx < BROWSER_MAX_LINKS; ni++) {
                dom_node_t* node = &browser_engine.nodes[ni];
                if (node->kind != DOM_ELEMENT) continue;
                if (node->href[0]) {
                    if (browser_str_equal(node->tag, "a") || browser_str_equal(node->tag, "A") ||
                        node->tag[0] == 'a' || node->tag[0] == 'A') {
                        browser_str_copy(browser.links[link_idx].url, node->href, BROWSER_URL_SIZE);
                        node->link_index = link_idx;
                        link_idx++;
                    }
                }
            }
            browser_engine.link_count = link_idx;
            css_apply_styles(&browser_engine);
            browser_engine.active = 1;
            serial_write("BROWSER: CSS engine active, dom nodes=");
            char nb[16];
            int_to_str(dom_nodes, nb);
            serial_write(nb);
            serial_write(", rules=");
            int_to_str(browser_engine.sheet.rule_count, nb);
            serial_write(nb);
            serial_write(", links=");
            int_to_str(link_idx, nb);
            serial_write(nb);
            serial_write("\n");
        } else {
            browser_engine.active = 0;
        }
        serial_write("BROWSER: content first 300:\n");
        int clen = browser.content_len > 300 ? 300 : browser.content_len;
        serial_write_hex((const uint8_t*)browser.content, clen);
        serial_write("\n");
        browser_set_status("完成");
        serial_write("BROWSER: loaded ");
        char lenbuf[16];
        int_to_str(n, lenbuf);
        serial_write(lenbuf);
        serial_write(" bytes, links=");
        int_to_str(browser.link_count, lenbuf);
        serial_write(lenbuf);
        serial_write("\n");
    } else {
        browser_str_copy(browser.content, "页面加载失败。\n请检查网络 / DNS / HTTP。", BROWSER_CONTENT_SIZE);
        browser.content_len = browser_str_len(browser.content);
        browser_set_status("错误");
    }
    browser.loading = 0;
    gui_dirty = 1;
}

void browser_navigate(const char* url) {
    browser_load(url, 1);
}

static void browser_go_back(void) {
    if (!browser.back_url[0]) return;
    char tmp[BROWSER_URL_SIZE];
    browser_str_copy(tmp, browser.back_url, BROWSER_URL_SIZE);
    browser_str_copy(browser.forward_url, browser.url, BROWSER_URL_SIZE);
    browser.back_url[0] = '\0';
    browser_load(tmp, 0);
}

static void browser_go_forward(void) {
    if (!browser.forward_url[0]) return;
    char tmp[BROWSER_URL_SIZE];
    browser_str_copy(tmp, browser.forward_url, BROWSER_URL_SIZE);
    browser_str_copy(browser.back_url, browser.url, BROWSER_URL_SIZE);
    browser.forward_url[0] = '\0';
    browser_load(tmp, 0);
}

static browser_link_t* browser_link_at(int idx) {
    for (int i = 0; i < browser.link_count; i++) {
        if (idx >= browser.links[i].start && idx < browser.links[i].end) return &browser.links[i];
    }
    return 0;
}

static int browser_total_rows(int cols) {
    if (browser.content_len == 0 || cols <= 0) return 0;
    int lines = 1;
    int cx = 0;
    int style = 0;
    int i = 0;
    while (i < browser.content_len) {
        char c = browser.content[i];
        if (c == '\x01') {
            if (i + 1 < browser.content_len) {
                style = (uint8_t)browser.content[i + 1];
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (c == '\n') {
            lines++;
            cx = 0;
            if (style == 1 || style == 2 || style == 3) lines++;
            i++;
            continue;
        }
        int adv = 1;
        uint8_t b0 = (uint8_t)c;
        if (b0 >= 0x80) {
            if ((b0 & 0xE0) == 0xC0) adv = 2;
            else if ((b0 & 0xF0) == 0xE0) adv = 3;
            else if ((b0 & 0xF8) == 0xF0) adv = 4;
        }
        cx++;
        if (cx > cols) {
            lines++;
            cx = 1;
        }
        i += adv;
    }
    return lines;
}

static int browser_max_scroll(int cols, int max_rows) {
    int total = browser_total_rows(cols);
    int v = total - max_rows;
    return v > 0 ? v : 0;
}

static void browser_clamp_scroll(int cols, int max_rows) {
    int max = browser_max_scroll(cols, max_rows);
    if (browser.scroll_y < 0) browser.scroll_y = 0;
    if (browser.scroll_y > max) browser.scroll_y = max;
}

void browser_handle_key(char c, uint8_t sc) {
    if (sc == 0x0E) {
        if (browser_url_len > 0) {
            browser_url_len--;
            browser.url[browser_url_len] = '\0';
            browser.url_focused = 1;
            gui_dirty = 1;
        }
    } else if (sc == 0x1C) {
        if (browser.url[0]) {
            if (!browser_str_starts_with(browser.url, "http://") &&
                !browser_str_starts_with(browser.url, "https://")) {
                char tmp[512];
                tmp[0]='h'; tmp[1]='t'; tmp[2]='t'; tmp[3]='p';
                tmp[4]='s'; tmp[5]=':'; tmp[6]='/'; tmp[7]='/';
                int i=0;
                while (browser.url[i] && i < 503) { tmp[8+i]=browser.url[i]; i++; }
                tmp[8+i]='\0';
                browser_navigate(tmp);
            } else {
                browser_navigate(browser.url);
            }
        }
    } else if (sc == 0x4B) {
        browser_go_back();
    } else if (sc == 0x4D) {
        browser_go_forward();
    } else if (sc == 0x48) {
        if (browser.url_focused) browser.url_focused = 0;
        browser.scroll_y--;
        gui_dirty = 1;
    } else if (sc == 0x50) {
        if (browser.url_focused) browser.url_focused = 0;
        browser.scroll_y++;
        gui_dirty = 1;
    } else if (sc == 0x49) {
        if (browser.url_focused) browser.url_focused = 0;
        browser.scroll_y -= 10;
        gui_dirty = 1;
    } else if (sc == 0x51) {
        if (browser.url_focused) browser.url_focused = 0;
        browser.scroll_y += 10;
        gui_dirty = 1;
    } else if (c && browser_url_len < BROWSER_URL_SIZE - 1) {
        browser.url[browser_url_len++] = c;
        browser.url[browser_url_len] = '\0';
        browser.url_focused = 1;
        gui_dirty = 1;
    }
}

static void __attribute__((unused)) browser_parse_url_local(const char* url, char* host, int host_max,
                                    char* path, int path_max) {
    host[0] = '\0';
    path[0] = '/';
    path[1] = '\0';
    const char* p = url;
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' && p[4] == ':') {
        p += 5;
        if (p[0] == '/' && p[1] == '/') p += 2;
    }
    int i = 0;
    while (p[i] && p[i] != '/' && p[i] != '?' && i < host_max - 1) {
        host[i] = p[i];
        i++;
    }
    host[i] = '\0';
    if (p[i]) {
        int j = 0;
        while (p[i] && j < path_max - 1) { path[j++] = p[i++]; }
        path[j] = '\0';
    }
}

static int browser_char_index_at_click(int mx, int my, int content_x, int content_y,
                                       int content_w, int content_h) {
    int cols = (content_w - 8) / FONT_WIDTH;
    int max_rows = content_h / FONT_HEIGHT;
    int skip_rows = browser.scroll_y;
    int cx = 0, cy = 0;
    int i = 0;
    while (i < browser.content_len) {
        char c = browser.content[i];
        if (c == '\x01') {
            if (i + 1 < browser.content_len) i += 2;
            else i++;
            continue;
        }
        if (c == '\n') { cx = 0; cy++; i++; continue; }
        if (cx >= cols) { cx = 0; cy++; }
        int adv = 1;
        uint8_t b0 = (uint8_t)c;
        if (b0 >= 0x80) {
            if ((b0 & 0xE0) == 0xC0) adv = 2;
            else if ((b0 & 0xF0) == 0xE0) adv = 3;
            else if ((b0 & 0xF8) == 0xF0) adv = 4;
        }
        if (cy < skip_rows) { cx++; i += adv; continue; }
        int ry = cy - skip_rows;
        if (ry >= max_rows) break;
        int px = content_x + 4 + cx * FONT_WIDTH;
        int py = content_y + 4 + ry * FONT_HEIGHT;
        if (mx >= px && mx < px + FONT_WIDTH && my >= py && my < py + FONT_HEIGHT) return i;
        cx++;
        i += adv;
    }
    return -1;
}

static void render_browser_dom_node(int node_idx, int content_x, int content_y,
                                     int content_w, int content_h, int scroll_off);
static int find_dom_text_at(int node_idx, int mx, int my, int scroll_off);
static int dom_total_height(int node_idx);

void browser_handle_click(window_t* win, int mx, int my) {
    if (!win) return;
    int x = win->x, y = win->y, w = win->w;
    int tab_y = y + 32;
    int tab_h = 26;
    int tab_w = 120;

    if (my >= tab_y && my < tab_y + tab_h) {
        for (int i = 0; i < browser_tabs.count && i < BROWSER_MAX_TABS; i++) {
            int tx = x + 8 + i * (tab_w + 2);
            int close_cx = tx + tab_w - 12;
            int close_cy = tab_y + tab_h / 2;
            if (mx >= close_cx - 5 && mx < close_cx + 5 && my >= close_cy - 5 && my < close_cy + 5) {
                browser_tab_close(i);
                return;
            }
            if (mx >= tx && mx < tx + tab_w) {
                browser_tab_select(i);
                return;
            }
        }
        if (browser_tabs.count < BROWSER_MAX_TABS) {
            int plus_x = x + 8 + browser_tabs.count * (tab_w + 2) + 4;
            if (mx >= plus_x && mx < plus_x + 16) {
                browser_tab_new();
                return;
            }
        }
    }

    int bar_y, url_x, url_w, back_x, fwd_x, refresh_x, home_x;
    browser_get_layout(win, &bar_y, &url_x, &url_w, &back_x, &fwd_x, &refresh_x, &home_x);
    int bar_h = 28;
    int btn_w = 28;

    if (my >= bar_y && my < bar_y + bar_h) {
        if (mx >= back_x && mx < back_x + btn_w) { browser_go_back(); return; }
        if (mx >= fwd_x && mx < fwd_x + btn_w) { browser_go_forward(); return; }
        if (mx >= refresh_x && mx < refresh_x + btn_w) { browser_navigate(browser.url); return; }
        if (mx >= home_x && mx < home_x + btn_w) { browser_home(); return; }
        if (mx >= url_x && mx < url_x + url_w) {
            browser.url_focused = 1;
            gui_dirty = 1;
            return;
        }
    }

    int content_y = bar_y + bar_h + 8;
    int status_h = 20;
    int content_h = win->h - (content_y - y) - status_h;
    int content_x = x + 8;
    int content_w = w - 28;
    int scroll_w = 12;
    int scroll_x = x + w - 8 - scroll_w;

    if (mx >= content_x && mx < content_x + content_w &&
        my >= content_y && my < content_y + content_h) {
        browser.url_focused = 0;
        if (browser_engine.active && browser_engine.root >= 0) {
            css_layout(&browser_engine, content_x + 4, content_y + 4, content_w - 8, content_h);
            int total_h = dom_total_height(browser_engine.root) - (content_y + 4);
            if (total_h < content_h) total_h = content_h;
            int max_scroll = total_h - content_h;
            if (max_scroll < 0) max_scroll = 0;
            if (browser.scroll_y > max_scroll) browser.scroll_y = max_scroll;
            int tidx = find_dom_text_at(browser_engine.root, mx, my, browser.scroll_y);
            if (tidx >= 0) {
                int pidx = browser_engine.nodes[tidx].parent;
                while (pidx >= 0) {
                    dom_node_t* pn = &browser_engine.nodes[pidx];
                    if (pn->link_index >= 0 && pn->href[0]) {
                        char resolved[512];
                        http_resolve_url_full(browser.url, pn->href, resolved, sizeof(resolved));
                        if (resolved[0]) browser_navigate(resolved);
                        break;
                    }
                    pidx = pn->parent;
                }
            }
        } else {
            int idx = browser_char_index_at_click(mx, my, content_x, content_y, content_w, content_h);
            browser_link_t* link = (idx >= 0) ? browser_link_at(idx) : 0;
            if (link) {
                char resolved[512];
                http_resolve_url_full(browser.url, link->url, resolved, sizeof(resolved));
                if (resolved[0]) browser_navigate(resolved);
            }
        }
        gui_dirty = 1;
    }

    if (mx >= scroll_x && mx < scroll_x + scroll_w &&
        my >= content_y && my < content_y + content_h) {
        browser.url_focused = 0;
        if (browser_engine.active && browser_engine.root >= 0) {
            css_layout(&browser_engine, content_x + 4, content_y + 4, content_w - 8, content_h);
            int total_h = dom_total_height(browser_engine.root) - (content_y + 4);
            if (total_h < content_h) total_h = content_h;
            int max_scroll = total_h - content_h;
            if (max_scroll < 0) max_scroll = 0;
            if (max_scroll > 0) {
                int rel = my - content_y;
                browser.scroll_y = rel * max_scroll / content_h;
                if (browser.scroll_y < 0) browser.scroll_y = 0;
                if (browser.scroll_y > max_scroll) browser.scroll_y = max_scroll;
            }
        } else {
            int cols = (content_w - 8) / FONT_WIDTH;
            int max_rows = content_h / FONT_HEIGHT;
            int total_rows = browser_total_rows(cols);
            int max_scroll = browser_max_scroll(cols, max_rows);
            if (max_scroll > 0 && total_rows > max_rows) {
                int rel = my - content_y;
                browser.scroll_y = rel * max_scroll / content_h;
                browser_clamp_scroll(cols, max_rows);
            }
        }
        gui_dirty = 1;
    }
}

static void render_browser_dom_node(int node_idx, int content_x, int content_y,
                                     int content_w, int content_h, int scroll_off) {
    dom_node_t* node = &browser_engine.nodes[node_idx];
    if (node->kind == DOM_TEXT) {
        if (node->text[0] == '\0') return;
        int py = node->y - scroll_off;
        if (py + FONT_HEIGHT < content_y || py > content_y + content_h) return;
        uint32_t color = node->color & 0xFFFFFF;
        if (color == 0) color = COLOR_TEXT_BLACK;
        int px = node->x;
        if (px < content_x) px = content_x;
        int is_link = (node->link_index >= 0);
        int parent_link = -1;
        int pidx = node->parent;
        while (pidx >= 0) {
            if (browser_engine.nodes[pidx].link_index >= 0) {
                parent_link = browser_engine.nodes[pidx].link_index;
                break;
            }
            pidx = browser_engine.nodes[pidx].parent;
        }
        if (parent_link >= 0) {
            color = 0x000000CC;
            is_link = 1;
        }
        int i = 0;
        int cx = node->x;
        while (node->text[i]) {
            uint8_t b0 = (uint8_t)node->text[i];
            uint32_t cp = 0;
            int adv = 1;
            int is_cn = 0;
            if (b0 >= 0x80) {
                if ((b0 & 0xE0) == 0xC0 && node->text[i+1]) {
                    cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)node->text[i+1] & 0x3F);
                    adv = 2; is_cn = 1;
                } else if ((b0 & 0xF0) == 0xE0 && node->text[i+1] && node->text[i+2]) {
                    cp = ((uint32_t)(b0 & 0x0F) << 12) |
                         ((uint32_t)((uint8_t)node->text[i+1] & 0x3F) << 6) |
                         ((uint8_t)node->text[i+2] & 0x3F);
                    adv = 3; is_cn = 1;
                } else if ((b0 & 0xF8) == 0xF0 && node->text[i+1] && node->text[i+2] && node->text[i+3]) {
                    cp = '?';
                    adv = 4;
                } else {
                    cp = '?';
                    adv = 1;
                }
            }
            if (cx + FONT_WIDTH > content_x + content_w) break;
            if (cx >= content_x && py >= content_y && py + FONT_HEIGHT <= content_y + content_h) {
                if (is_cn) {
                    if (node->font_bold) draw_char_cn_bold(cx, py, (uint16_t)cp, color);
                    else draw_char_cn(cx, py, (uint16_t)cp, color);
                } else {
                    char ch = cp ? (char)(cp & 0xFF) : (char)node->text[i];
                    if (node->font_bold) draw_char_bold(cx, py, ch, color);
                    else draw_char(cx, py, ch, color);
                }
                if (is_link) {
                    fb_fillrect(cx, py + FONT_HEIGHT - 1, FONT_WIDTH, 1, color);
                }
            }
            cx += FONT_WIDTH;
            i += adv;
        }
        return;
    }
    if (node->kind == DOM_ELEMENT) {
        if (node->bg_color != 0 && node->content_w > 0 && node->content_h > 0) {
            int bx = node->content_x;
            int by = node->content_y - scroll_off;
            int bw = node->content_w;
            int bh = node->content_h;
            if (by + bh >= content_y && by <= content_y + content_h) {
                fb_fillrect(bx, by, bw, bh, node->bg_color & 0xFFFFFF);
            }
        }
        for (int c = 0; c < node->child_count; c++) {
            render_browser_dom_node(node->children[c], content_x, content_y, content_w, content_h, scroll_off);
        }
    }
}

static int find_dom_text_at(int node_idx, int mx, int my, int scroll_off) {
    dom_node_t* node = &browser_engine.nodes[node_idx];
    if (node->kind == DOM_TEXT) {
        if (node->text[0] == '\0') return -1;
        int py = node->y - scroll_off;
        if (my < py || my >= py + FONT_HEIGHT) return -1;
        int cx = node->x;
        int i = 0;
        while (node->text[i]) {
            uint8_t b0 = (uint8_t)node->text[i];
            int adv = 1;
            if (b0 >= 0x80) {
                if ((b0 & 0xE0) == 0xC0) adv = 2;
                else if ((b0 & 0xF0) == 0xE0) adv = 3;
                else if ((b0 & 0xF8) == 0xF0) adv = 4;
            }
            if (mx >= cx && mx < cx + FONT_WIDTH) return node_idx;
            cx += FONT_WIDTH;
            i += adv;
        }
        return -1;
    }
    for (int c = 0; c < node->child_count; c++) {
        int r = find_dom_text_at(node->children[c], mx, my, scroll_off);
        if (r >= 0) return r;
    }
    return -1;
}

static int dom_total_height(int node_idx) {
    dom_node_t* node = &browser_engine.nodes[node_idx];
    int max_y = node->y + node->h;
    for (int c = 0; c < node->child_count; c++) {
        int h = dom_total_height(node->children[c]);
        if (h > max_y) max_y = h;
    }
    return max_y;
}

static void render_browser_dom(window_t* win) {
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int tab_h = 26;
    int tab_y = y + 32;
    int tab_w = 120;
    int mx = mouse_x, my = mouse_y;
    for (int i = 0; i < browser_tabs.count && i < BROWSER_MAX_TABS; i++) {
        int tx = x + 8 + i * (tab_w + 2);
        int is_active = (i == browser_tabs.active);
        uint32_t bg = is_active ? g_theme.bg : g_theme.border;
        fb_fillrect(tx, tab_y, tab_w, tab_h, bg);
        if (is_active) {
            fb_fillrect(tx, tab_y, tab_w, 2, g_theme.accent);
        }
        const char* url = browser_tabs.url[i][0] ? browser_tabs.url[i] : "新标签页";
        int slen = 0;
        while (url[slen] && slen < 12) slen++;
        char tname[16];
        for (int k = 0; k < slen; k++) tname[k] = url[k];
        tname[slen] = '\0';
        draw_string(tx + 6, tab_y + 5, tname, is_active ? g_theme.text : g_theme.text_secondary);
        int close_cx = tx + tab_w - 12;
        int close_cy = tab_y + tab_h / 2;
        if (mx >= close_cx - 4 && mx < close_cx + 4 && my >= close_cy - 4 && my < close_cy + 4) {
            fb_fill_circle(close_cx, close_cy, 6, 0x66CC4444);
        }
        draw_browser_icon_close(close_cx, close_cy, g_theme.text);
    }
    if (browser_tabs.count < BROWSER_MAX_TABS) {
        int plus_x = x + 8 + browser_tabs.count * (tab_w + 2) + 4;
        int plus_cx = plus_x + 8;
        int plus_cy = tab_y + tab_h / 2;
        int plus_hover = (mx >= plus_x && mx < plus_x + 16 && my >= tab_y && my < tab_y + tab_h);
        if (plus_hover) fb_fillrect(plus_x, tab_y, 16, tab_h, g_theme.border);
        draw_browser_icon_plus(plus_cx, plus_cy, g_theme.text);
    }

    int bar_y = tab_y + tab_h + 2;
    int bar_h = 28;
    int btn_w = 28;
    int btn_h = bar_h;
    int back_x = x + 8;
    int fwd_x = back_x + btn_w + 4;
    int refresh_x = fwd_x + btn_w + 4;
    int home_x = refresh_x + btn_w + 4;
    int url_x = home_x + btn_w + 8;
    int url_w = x + w - 8 - url_x;

    int back_hover = (mx >= back_x && mx < back_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(back_x, bar_y, btn_w, btn_h, back_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_back(back_x + btn_w / 2, bar_y + btn_h / 2,
                          browser.back_url[0] ? g_theme.text : g_theme.text_secondary);
    int fwd_hover = (mx >= fwd_x && mx < fwd_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(fwd_x, bar_y, btn_w, btn_h, fwd_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_forward(fwd_x + btn_w / 2, bar_y + btn_h / 2,
                             browser.forward_url[0] ? g_theme.text : g_theme.text_secondary);
    int ref_hover = (mx >= refresh_x && mx < refresh_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(refresh_x, bar_y, btn_w, btn_h, ref_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_refresh(refresh_x + btn_w / 2, bar_y + btn_h / 2, g_theme.text);
    int home_hover = (mx >= home_x && mx < home_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(home_x, bar_y, btn_w, btn_h, home_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_home(home_x + btn_w / 2, bar_y + btn_h / 2, g_theme.text);

    fb_fillrect(url_x, bar_y, url_w, btn_h, COLOR_TEXT_WHITE);
    uint32_t url_border = browser.url_focused ? g_theme.accent : g_theme.border;
    fb_draw_round_rect(url_x, bar_y, url_w, btn_h, 6, url_border);
    draw_string(url_x + 8, bar_y + 5, browser.url, g_theme.text);
    if (browser.url_focused && (timer_ticks / 25) % 2 == 0) {
        int cur_x = url_x + 8 + browser_url_len * FONT_WIDTH;
        fb_fillrect(cur_x, bar_y + 4, 2, btn_h - 8, g_theme.text);
    }
    if (browser.loading) {
        int prog = (timer_ticks * 4) % (url_w - 16);
        if (prog < 0) prog += url_w - 16;
        int pw = 40;
        if (prog + pw > url_w - 4) pw = (url_w - 4) - prog;
        if (pw > 0) fb_fillrect(url_x + 4 + prog, bar_y, pw, 2, g_theme.accent);
    }

    int content_y = bar_y + bar_h + 8;
    int status_h = 20;
    int content_h = h - (content_y - y) - status_h;
    if (content_h < 0) content_h = 0;
    int content_x = x + 8;
    int content_w = w - 28;
    int scroll_w = 12;
    int scroll_x = x + w - 8 - scroll_w;

    fb_fillrect(content_x, content_y, content_w, content_h, COLOR_TEXT_WHITE);
    fb_set_clip(content_x, content_y, content_w, content_h);

    css_layout(&browser_engine, content_x + 4, content_y + 4, content_w - 8, content_h);

    int total_h = dom_total_height(browser_engine.root) - (content_y + 4);
    if (total_h < content_h) total_h = content_h;
    int max_scroll = total_h - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (browser.scroll_y < 0) browser.scroll_y = 0;
    if (browser.scroll_y > max_scroll) browser.scroll_y = max_scroll;
    int scroll_off = browser.scroll_y;

    render_browser_dom_node(browser_engine.root, content_x, content_y, content_w, content_h, scroll_off);

    if (mx >= content_x && mx < content_x + content_w && my >= content_y && my < content_y + content_h) {
        int tidx = find_dom_text_at(browser_engine.root, mx, my, scroll_off);
        browser_hover_url[0] = '\0';
        if (tidx >= 0) {
            int pidx = browser_engine.nodes[tidx].parent;
            while (pidx >= 0) {
                dom_node_t* pn = &browser_engine.nodes[pidx];
                if (pn->link_index >= 0 && pn->href[0]) {
                    int k = 0;
                    while (pn->href[k] && k < BROWSER_URL_SIZE - 1) { browser_hover_url[k] = pn->href[k]; k++; }
                    browser_hover_url[k] = '\0';
                    break;
                }
                pidx = pn->parent;
            }
        }
    } else {
        browser_hover_url[0] = '\0';
    }

    fb_reset_clip();

    if (total_h > content_h) {
        fb_fillrect(scroll_x, content_y, scroll_w, content_h, g_theme.border);
        int thumb_h = content_h * content_h / total_h;
        if (thumb_h < 16) thumb_h = 16;
        int thumb_y = content_y;
        if (max_scroll > 0) {
            thumb_y += (content_h - thumb_h) * browser.scroll_y / max_scroll;
        }
        fb_fillrect(scroll_x, thumb_y, scroll_w, thumb_h, g_theme.text_secondary);
    } else {
        fb_fillrect(scroll_x, content_y, scroll_w, content_h, g_theme.border);
    }

    int status_y = y + h - 16;
    fb_fillrect(x, status_y - 2, w, status_h, g_theme.border);
    if (browser_hover_url[0]) {
        draw_string(x + 8, status_y, browser_hover_url, g_theme.accent);
    } else {
        draw_string(x + 8, status_y, browser.status, g_theme.text_secondary);
    }
}

void render_browser(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    if (browser_engine.active) {
        render_browser_dom(win);
        return;
    }
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int tab_h = 26;
    int tab_y = y + 32;
    int tab_w = 120;
    int mx = mouse_x, my = mouse_y;
    for (int i = 0; i < browser_tabs.count && i < BROWSER_MAX_TABS; i++) {
        int tx = x + 8 + i * (tab_w + 2);
        int is_active = (i == browser_tabs.active);
        uint32_t bg = is_active ? g_theme.bg : g_theme.border;
        fb_fillrect(tx, tab_y, tab_w, tab_h, bg);
        if (is_active) fb_fillrect(tx, tab_y, tab_w, 2, g_theme.accent);
        const char* url = browser_tabs.url[i][0] ? browser_tabs.url[i] : "新标签页";
        int slen = 0;
        while (url[slen] && slen < 12) slen++;
        char tname[16];
        for (int k = 0; k < slen; k++) tname[k] = url[k];
        tname[slen] = '\0';
        draw_string(tx + 6, tab_y + 5, tname, is_active ? g_theme.text : g_theme.text_secondary);
        int close_cx = tx + tab_w - 12;
        int close_cy = tab_y + tab_h / 2;
        if (mx >= close_cx - 4 && mx < close_cx + 4 && my >= close_cy - 4 && my < close_cy + 4) {
            fb_fill_circle(close_cx, close_cy, 6, 0x66CC4444);
        }
        draw_browser_icon_close(close_cx, close_cy, g_theme.text);
    }
    if (browser_tabs.count < BROWSER_MAX_TABS) {
        int plus_x = x + 8 + browser_tabs.count * (tab_w + 2) + 4;
        int plus_cx = plus_x + 8;
        int plus_cy = tab_y + tab_h / 2;
        int plus_hover = (mx >= plus_x && mx < plus_x + 16 && my >= tab_y && my < tab_y + tab_h);
        if (plus_hover) fb_fillrect(plus_x, tab_y, 16, tab_h, g_theme.border);
        draw_browser_icon_plus(plus_cx, plus_cy, g_theme.text);
    }

    int bar_y = tab_y + tab_h + 2;
    int bar_h = 28;
    int btn_w = 28;
    int btn_h = bar_h;
    int back_x = x + 8;
    int fwd_x = back_x + btn_w + 4;
    int refresh_x = fwd_x + btn_w + 4;
    int home_x = refresh_x + btn_w + 4;
    int url_x = home_x + btn_w + 8;
    int url_w = x + w - 8 - url_x;

    int back_hover = (mx >= back_x && mx < back_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(back_x, bar_y, btn_w, btn_h, back_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_back(back_x + btn_w / 2, bar_y + btn_h / 2,
                          browser.back_url[0] ? g_theme.text : g_theme.text_secondary);
    int fwd_hover = (mx >= fwd_x && mx < fwd_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(fwd_x, bar_y, btn_w, btn_h, fwd_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_forward(fwd_x + btn_w / 2, bar_y + btn_h / 2,
                             browser.forward_url[0] ? g_theme.text : g_theme.text_secondary);
    int ref_hover = (mx >= refresh_x && mx < refresh_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(refresh_x, bar_y, btn_w, btn_h, ref_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_refresh(refresh_x + btn_w / 2, bar_y + btn_h / 2, g_theme.text);
    int home_hover = (mx >= home_x && mx < home_x + btn_w && my >= bar_y && my < bar_y + btn_h);
    fb_fillrect(home_x, bar_y, btn_w, btn_h, home_hover ? g_theme.border : g_theme.bg);
    draw_browser_icon_home(home_x + btn_w / 2, bar_y + btn_h / 2, g_theme.text);

    fb_fillrect(url_x, bar_y, url_w, btn_h, COLOR_TEXT_WHITE);
    uint32_t url_border = browser.url_focused ? g_theme.accent : g_theme.border;
    fb_draw_round_rect(url_x, bar_y, url_w, btn_h, 6, url_border);
    draw_string(url_x + 8, bar_y + 5, browser.url, g_theme.text);
    if (browser.url_focused && (timer_ticks / 25) % 2 == 0) {
        int cur_x = url_x + 8 + browser_url_len * FONT_WIDTH;
        fb_fillrect(cur_x, bar_y + 4, 2, btn_h - 8, g_theme.text);
    }
    if (browser.loading) {
        int prog = (timer_ticks * 4) % (url_w - 16);
        if (prog < 0) prog += url_w - 16;
        int pw = 40;
        if (prog + pw > url_w - 4) pw = (url_w - 4) - prog;
        if (pw > 0) fb_fillrect(url_x + 4 + prog, bar_y, pw, 2, g_theme.accent);
    }

    int content_y = bar_y + bar_h + 8;
    int status_h = 20;
    int content_h = h - (content_y - y) - status_h;
    if (content_h < 0) content_h = 0;
    int content_x = x + 8;
    int content_w = w - 28;
    int scroll_w = 12;
    int scroll_x = x + w - 8 - scroll_w;

    fb_fillrect(content_x, content_y, content_w, content_h, COLOR_TEXT_WHITE);
    fb_set_clip(content_x, content_y, content_w, content_h);

    int cols = (content_w - 8) / FONT_WIDTH;
    int max_rows = content_h / FONT_HEIGHT;
    browser_clamp_scroll(cols, max_rows);
    int skip_rows = browser.scroll_y;

    int cx = 0, cy = 0;
    int rows_drawn = 0;
    int style = 0;
    int i = 0;
    while (i < browser.content_len && rows_drawn < max_rows) {
        char c = browser.content[i];
        if (c == '\x01') {
            if (i + 1 < browser.content_len) {
                style = (uint8_t)browser.content[i + 1];
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (c == '\n') {
            cx = 0;
            cy++;
            if (style == 1 || style == 2 || style == 3) cy++;
            i++;
            continue;
        }
        if (cx >= cols) {
            cx = 0;
            cy++;
        }
        uint32_t ch_cp = 0;
        int is_cn = 0;
        int adv = 1;
        uint8_t b0 = (uint8_t)c;
        if (b0 >= 0x80) {
            uint32_t cp = 0;
            if ((b0 & 0xE0) == 0xC0 && i + 1 < browser.content_len) {
                cp = ((uint32_t)(b0 & 0x1F) << 6) | ((uint8_t)browser.content[i + 1] & 0x3F);
                adv = 2;
            } else if ((b0 & 0xF0) == 0xE0 && i + 2 < browser.content_len) {
                cp = ((uint32_t)(b0 & 0x0F) << 12) |
                     ((uint32_t)((uint8_t)browser.content[i + 1] & 0x3F) << 6) |
                     ((uint8_t)browser.content[i + 2] & 0x3F);
                adv = 3;
            } else if ((b0 & 0xF8) == 0xF0 && i + 3 < browser.content_len) {
                cp = ((uint32_t)(b0 & 0x07) << 18) |
                     ((uint32_t)((uint8_t)browser.content[i + 1] & 0x3F) << 12) |
                     ((uint32_t)((uint8_t)browser.content[i + 2] & 0x3F) << 6) |
                     ((uint8_t)browser.content[i + 3] & 0x3F);
                adv = 4;
            } else {
                cp = '?';
                adv = 1;
            }
            if (cp < 0x10000) {
                ch_cp = cp;
                is_cn = 1;
            } else {
                ch_cp = '?';
                is_cn = 0;
            }
        }
        if (cy < skip_rows) {
            cx++;
            i += adv;
            continue;
        }
        int ry = cy - skip_rows;
        if (ry >= max_rows) break;
        uint32_t color = COLOR_TEXT_BLACK;
        int use_bold = 0;
        int indent = 0;
        switch (style) {
            case 1: use_bold = 1; color = 0x001A1A2E; break;
            case 2: use_bold = 1; color = 0x001A1A2E; break;
            case 3: use_bold = 1; color = 0x00333333; break;
            case 5: use_bold = 1; color = 0x00000000; break;
            case 6: color = 0x00555555; break;
            case 7: color = 0x00880000; break;
            case 8: color = 0x00555555; indent = 2; break;
            case 9: color = 0x00000000; break;
            default: color = COLOR_TEXT_BLACK; break;
        }
        browser_link_t* link = browser_link_at(i);
        if (link) {
            color = 0x000000CC;
        }
        int px = content_x + 4 + (cx + indent) * FONT_WIDTH;
        int py = content_y + 4 + ry * FONT_HEIGHT;
        if (is_cn) {
            if (use_bold) {
                draw_char_cn_bold(px, py, (uint16_t)ch_cp, color);
            } else {
                draw_char_cn(px, py, (uint16_t)ch_cp, color);
            }
        } else {
            if (use_bold) {
                draw_char_bold(px, py, c, color);
            } else {
                draw_char(px, py, c, color);
            }
        }
        if (link) fb_fillrect(px, py + FONT_HEIGHT - 1, FONT_WIDTH, 1, 0x000000CC);
        cx++;
        rows_drawn = ry + 1;
        i += adv;
    }

    fb_reset_clip();

    int total_rows = browser_total_rows(cols);
    if (total_rows > max_rows && max_rows > 0) {
        int track_y = content_y;
        int track_h = content_h;
        fb_fillrect(scroll_x, track_y, scroll_w, track_h, 0x00DDDDDD);
        int thumb_h = track_h * max_rows / total_rows;
        if (thumb_h < 16) thumb_h = 16;
        int max_scroll = browser_max_scroll(cols, max_rows);
        int thumb_y = track_y;
        if (max_scroll > 0) {
            thumb_y += (track_h - thumb_h) * browser.scroll_y / max_scroll;
        }
        fb_fillrect(scroll_x, thumb_y, scroll_w, thumb_h, 0x00888888);
    } else {
        fb_fillrect(scroll_x, content_y, scroll_w, content_h, 0x00DDDDDD);
    }

    if (mx >= content_x && mx < content_x + content_w && my >= content_y && my < content_y + content_h) {
        int hidx = browser_char_index_at_click(mx, my, content_x, content_y, content_w, content_h);
        browser_link_t* hlink = (hidx >= 0) ? browser_link_at(hidx) : 0;
        if (hlink) {
            int k = 0;
            while (hlink->url[k] && k < BROWSER_URL_SIZE - 1) { browser_hover_url[k] = hlink->url[k]; k++; }
            browser_hover_url[k] = '\0';
        } else {
            browser_hover_url[0] = '\0';
        }
    } else {
        browser_hover_url[0] = '\0';
    }

    int status_y = y + h - 16;
    fb_fillrect(x, status_y - 2, w, 20, g_theme.border);
    if (browser_hover_url[0]) {
        draw_string(x + 8, status_y, browser_hover_url, g_theme.accent);
    } else {
        draw_string(x + 8, status_y, browser.status, g_theme.text_secondary);
    }
}
