#include "gui_internal.h"
#include "idt.h"
#include "auth.h"
#include "mouse.h"
#include "io.h"
#include "theme.h"
#include "serial.h"
#include <stdint.h>

static char login_password[32];
static int login_password_len = 0;
static int login_anim_frame = 0;
static int login_failed = 0;
int login_anim_active = 0;

static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t bcd_to_bin(uint8_t bcd) {
    return (uint8_t)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static void rtc_read(int* hour, int* minute, int* second,
                     int* year, int* month, int* day, int* weekday) {
    uint8_t regB = cmos_read(0x0B);
    int is_binary = (regB & 0x04) ? 1 : 0;

    uint8_t s = cmos_read(0x00);
    uint8_t m = cmos_read(0x02);
    uint8_t h = cmos_read(0x04);
    uint8_t wd = cmos_read(0x06);
    uint8_t d = cmos_read(0x07);
    uint8_t mo = cmos_read(0x08);
    uint8_t y = cmos_read(0x09);
    uint8_t c = cmos_read(0x32);

    if (!is_binary) {
        s = bcd_to_bin(s); m = bcd_to_bin(m); h = bcd_to_bin(h);
        wd = bcd_to_bin(wd); d = bcd_to_bin(d); mo = bcd_to_bin(mo);
        y = bcd_to_bin(y); c = bcd_to_bin(c);
    }
    *second = s; *minute = m; *hour = h; *weekday = wd;
    *day = d; *month = mo;
    *year = (int)c * 100 + (int)y;
    if (*year < 2000) *year += 2000;
}

static char* append_str(char* p, const char* s) {
    while (*s) *p++ = *s++;
    return p;
}

static char* append_int(char* p, int v) {
    char buf[12];
    int_to_str(v, buf);
    return append_str(p, buf);
}

void render_lockscreen(void) {
    int sw = (int)fb_width;
    int sh = (int)fb_height;

    fb_fillrect_grad(0, 0, sw, sh, 0x0008101A, 0x00142740);
    fb_fill_circle(sw / 3, sh / 3, sh / 4, 0x00284A78);
    fb_fill_circle(sw * 2 / 3, sh * 2 / 3, sh / 5, 0x00153860);
    fb_box_blur(0, 0, sw, sh, 2);

    int hh, mm, ss, year, mon, day, wd;
    rtc_read(&hh, &mm, &ss, &year, &mon, &day, &wd);

    char clk[8];
    clk[0] = '0' + (hh / 10) % 10;
    clk[1] = '0' + hh % 10;
    clk[2] = ':';
    clk[3] = '0' + (mm / 10) % 10;
    clk[4] = '0' + mm % 10;
    clk[5] = 0;

    int char_w = FONT_WIDTH * 2;
    int clk_w = 5 * char_w;
    int tx = (sw - clk_w) / 2;
    int ty = sh - 200;
    for (int i = 0; i < 5; i++) {
        draw_char_2x(tx + i * char_w, ty, clk[i], COLOR_TEXT_WHITE);
        draw_char_2x(tx + i * char_w + 1, ty, clk[i], COLOR_TEXT_WHITE);
    }

    static const char* wnames[] = {
        "日","一","二","三","四","五","六"
    };
    int widx = wd - 1;
    if (widx < 0 || widx > 6) widx = 0;
    char date[48];
    char* p = date;
    p = append_int(p, year);
    p = append_str(p, "年");
    p = append_int(p, mon);
    p = append_str(p, "月");
    p = append_int(p, day);
    p = append_str(p, "日 星期");
    p = append_str(p, wnames[widx]);
    *p = 0;

    int date_w = string_width(date);
    draw_string_bold((sw - date_w) / 2, ty + FONT_HEIGHT * 2 + 16, date, COLOR_TEXT_WHITE);

    const char* hint = "按任意键登录";
    int hint_w = string_width(hint);
    draw_string(sw - hint_w - 24, sh - 40, hint, COLOR_TEXT_WHITE);
}

void render_login(void) {
    int sw = (int)fb_width;
    int sh = (int)fb_height;

    if (login_anim_active) {
        serial_write("LOGIN: anim frame, switching to desktop\n");
        gui_state = GUI_STATE_DESKTOP;
        login_anim_active = 0;
        login_anim_frame = 0;
        gui_dirty = 1;
        fb_blit_dirty();
        dirty_clear();
        return;
    }

    fb_fillrect_grad(0, 0, sw, sh, 0x0008101A, 0x00142740);

    int pw = 360;
    int ph = 480;
    int px = (sw - pw) / 2;
    int py = (sh - ph) / 2;

    fb_draw_round_rect(px + 3, py + 6, pw, ph, 12, 0x00404040);
    fb_draw_round_rect(px, py, pw, ph, 12, 0x00F5F5F5);

    int av_cx = px + pw / 2;
    int av_cy = py + 80;
    fb_fill_circle(av_cx, av_cy, 50, 0x00CCCCCC);
    fb_fill_circle(av_cx, av_cy, 48, 0x00BBBBBB);
    draw_icon_user(av_cx - 16, av_cy - 16);

    draw_string_centered_bold(px, py + 150, pw, "root", COLOR_TEXT_BLACK);

    int input_x = px + 30;
    int input_y = py + 200;
    int input_w = pw - 60;
    int input_h = 36;
    fb_draw_round_rect(input_x - 1, input_y - 1, input_w + 2, input_h + 2, 6, g_theme.accent);
    fb_draw_round_rect(input_x, input_y, input_w, input_h, 5, 0x00FFFFFF);

    for (int i = 0; i < login_password_len; i++) {
        fb_fill_circle(input_x + 14 + i * 14, input_y + input_h / 2, 4, COLOR_TEXT_BLACK);
    }

    int btn_x = px + 30;
    int btn_y = py + 260;
    int btn_w = pw - 60;
    int btn_h = 40;
    int mx = mouse_x, my = mouse_y;
    int hover = (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h);
    uint32_t btn_color = hover ? g_theme.accent_hover : g_theme.accent;
    fb_draw_round_rect(btn_x, btn_y, btn_w, btn_h, 6, btn_color);
    draw_string_centered_bold(btn_x, btn_y + (btn_h - FONT_HEIGHT) / 2, btn_w, "登录", COLOR_TEXT_WHITE);

    if (login_failed) {
        draw_string_centered(px, py + 320, pw, "登录失败，请重试", COLOR_CLOSE_BTN);
    } else {
        draw_string_centered(px, py + 320, pw, "按 Enter 键登录", COLOR_TEXT_GRAY);
    }
}

void login_handle_key(char c, uint8_t sc) {
    if (sc == 0x1C) {
        login_password[login_password_len] = 0;
        if (auth_login("root", login_password) >= 0) {
            login_anim_active = 1;
            login_anim_frame = 0;
            login_password_len = 0;
            login_failed = 0;
        } else {
            login_password_len = 0;
            login_failed = 1;
        }
    } else if (sc == 0x0E) {
        if (login_password_len > 0) login_password_len--;
    } else if (c && login_password_len < 31) {
        login_password[login_password_len++] = c;
    }
}