#include "gui_internal.h"
#include "systray.h"
#include "audio.h"
#include "easing.h"
#include "net.h"
#include "idt.h"
#include "mouse.h"
#include "gfx_settings.h"
#include "theme.h"
#include <stdint.h>

#define ICON_BOX   28
#define ICON_GAP    6
#define POPUP_GAP   4

#define VOL_POPUP_W 280
#define VOL_POPUP_H  72
#define NET_POPUP_W 280
#define NET_POPUP_H 104
#define CAL_POPUP_W 280
#define CAL_POPUP_H 248

#define CTX_MENU_ITEMS 2
#define CTX_MENU_W    150
#define CTX_MENU_ITEM_H 28

static systray_icon_t s_icons[SYSTRAY_ICON_COUNT];
static int s_vol_dragging = 0;
static int s_vol_cached = 75;

static int s_ctx_visible = 0;
static int s_ctx_x = 0;
static int s_ctx_y = 0;
static int s_ctx_sel = -1;

static void str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_month(int y, int m) {
    static const int dm[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && is_leap(y)) return 29;
    return dm[m - 1];
}

static uint32_t days_from_epoch(int y, int m, int d) {
    uint32_t days = 0;
    for (int yy = 2025; yy < y; yy++) days += is_leap(yy) ? 366 : 365;
    for (int mm = 1; mm < m; mm++) days += (uint32_t)days_in_month(y, mm);
    days += (uint32_t)(d - 1);
    return days;
}

static int weekday_of(int y, int m, int d) {
    return (int)((3u + days_from_epoch(y, m, d)) % 7u);
}

static void ticks_to_date(uint32_t ticks, int* yy, int* mm, int* dd,
                          int* hh, int* mi, int* ss) {
    uint32_t secs = ticks / 50u;
    *ss = (int)(secs % 60u); secs /= 60u;
    *mi = (int)(secs % 60u); secs /= 60u;
    *hh = (int)(secs % 24u); secs /= 24u;
    uint32_t days = secs;
    int y = 2025, m = 1;
    while (1) {
        int dy = is_leap(y) ? 366 : 365;
        if (days >= (uint32_t)dy) { days -= (uint32_t)dy; y++; }
        else break;
    }
    while (1) {
        int dm = days_in_month(y, m);
        if (days >= (uint32_t)dm) { days -= (uint32_t)dm; m++; if (m > 12) { m = 1; y++; } }
        else break;
    }
    *yy = y; *mm = m; *dd = (int)days + 1;
}

static void draw_arc_right(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - 1) * (r - 1) && dx >= 0)
                fb_putpixel(cx + dx, cy + dy, color);
        }
    }
}

static void draw_circle_outline(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r && d2 >= (r - 1) * (r - 1))
                fb_putpixel(cx + dx, cy + dy, color);
        }
    }
}

static void draw_volume_glyph(int bx, int by, uint32_t color) {
    int x = bx + 4, y = by + 4;
    fb_fillrect(x + 2, y + 8, 5, 6, color);
    for (int i = 0; i <= 5; i++) {
        int yu = y + 9 - i;
        int yd = y + 9 + i;
        int xr = x + 7 + i;
        fb_draw_line(x + 7, yu, xr, yu, color);
        fb_draw_line(x + 7, yd, xr, yd, color);
    }
    draw_arc_right(x + 15, y + 9, 3, color);
    draw_arc_right(x + 15, y + 9, 5, color);
}

static void draw_network_glyph(int bx, int by, uint32_t color) {
    int x = bx + 5, y = by + 4;
    int bottom = y + 16;
    int heights[4] = {4, 8, 12, 16};
    int xx = x;
    for (int i = 0; i < 4; i++) {
        fb_fillrect(xx, bottom - heights[i], 3, heights[i], color);
        xx += 5;
    }
}

static void draw_clock_glyph(int bx, int by, uint32_t color) {
    int cx = bx + 14, cy = by + 14;
    draw_circle_outline(cx, cy, 8, color);
    fb_draw_line(cx, cy, cx, cy - 4, color);
    fb_draw_line(cx, cy, cx + 5, cy, color);
    fb_putpixel(cx, cy, color);
}

static void draw_icon(systray_icon_t* ic) {
    if (ic->hover || ic->popup_open)
        fb_fillrect(ic->x, ic->y, ICON_BOX, ICON_BOX, COLOR_TASKBAR_HOVER);
    uint32_t col = COLOR_TEXT_WHITE;
    if (ic->type == SYSTRAY_VOLUME) draw_volume_glyph(ic->x, ic->y, col);
    else if (ic->type == SYSTRAY_NETWORK) draw_network_glyph(ic->x, ic->y, col);
    else draw_clock_glyph(ic->x, ic->y, col);
}

static int icon_hit(systray_icon_t* ic, int mx, int my) {
    return mx >= ic->x && mx < ic->x + ICON_BOX &&
           my >= ic->y && my < ic->y + ICON_BOX;
}

static systray_icon_t* icon_at(int mx, int my) {
    for (int i = 0; i < SYSTRAY_ICON_COUNT; i++)
        if (icon_hit(&s_icons[i], mx, my)) return &s_icons[i];
    return 0;
}

static void close_all_popups(void) {
    for (int i = 0; i < SYSTRAY_ICON_COUNT; i++) s_icons[i].popup_open = 0;
}

static void shadow_rect(int x, int y, int w, int h) {
    fb_fillrect(x + 3, y + 3, w, h, 0x55000000);
}

static int popup_right_edge(void) {
    return (int)fb_width - 8;
}

static int popup_x(int w) {
    int x = popup_right_edge() - w;
    if (x < 8) x = 8;
    return x;
}

static int popup_y(int h) {
    int y = (int)fb_height - TASKBAR_HEIGHT - h - POPUP_GAP;
    if (y < 8) y = 8;
    return y;
}

static void draw_card_bg(int x, int y, int w, int h) {
    shadow_rect(x, y, w, h);
    fb_draw_round_rect(x, y, w, h, 6, g_theme.bg);
    fb_draw_rect_outline(x, y, w, h, g_theme.border);
}

static void render_volume_popup(void) {
    int w = VOL_POPUP_W, h = VOL_POPUP_H;
    int x = popup_x(w), y = popup_y(h);
    draw_card_bg(x, y, w, h);
    draw_string_bold(x + 12, y + 8, "Volume", g_theme.text);

    int vol = audio_get_volume();
    if (s_vol_dragging) vol = s_vol_cached;
    char pct[8];
    int_to_str(vol, pct);
    int pl = 0;
    while (pct[pl]) pl++;
    pct[pl++] = '%';
    pct[pl] = '\0';
    draw_string(x + w - 12 - string_width(pct), y + 8, pct, g_theme.text);

    int tx = x + 16, ty = y + 44, tw = w - 32;
    fb_fillrect(tx, ty, tw, 6, g_theme.border);
    int fw = tw * vol / 100;
    if (fw > 0) fb_fillrect(tx, ty, fw, 6, g_theme.accent);
    int kx = tx + fw;
    fb_fill_circle(kx, ty + 3, 7, g_theme.accent);
    draw_circle_outline(kx, ty + 3, 7, g_theme.text);
}

static void render_network_popup(void) {
    int w = NET_POPUP_W, h = NET_POPUP_H;
    int x = popup_x(w), y = popup_y(h);
    draw_card_bg(x, y, w, h);
    draw_string_bold(x + 12, y + 8, "Network", g_theme.text);

    int connected = (local_ip != 0) ? 1 : 0;
    const char* status = connected ? "Status: Connected" : "Status: Offline";
    draw_string(x + 12, y + 36, status, g_theme.text_secondary);

    char ipbuf[32];
    if (connected) net_ip_to_str(local_ip, ipbuf);
    else str_copy(ipbuf, "0.0.0.0", 32);
    char line[64];
    str_copy(line, "IP: ", 64);
    int ll = 0;
    while (line[ll]) ll++;
    int j = 0;
    while (ipbuf[j] && ll < 60) { line[ll++] = ipbuf[j++]; }
    line[ll] = '\0';
    draw_string(x + 12, y + 60, line, g_theme.text_secondary);

    char gwbuf[32];
    if (gateway_ip) net_ip_to_str(gateway_ip, gwbuf);
    else str_copy(gwbuf, "0.0.0.0", 32);
    str_copy(line, "Gateway: ", 64);
    ll = 0;
    while (line[ll]) ll++;
    j = 0;
    while (gwbuf[j] && ll < 60) { line[ll++] = gwbuf[j++]; }
    line[ll] = '\0';
    draw_string(x + 12, y + 84, line, g_theme.text_secondary);
}

static void render_clock_popup(void) {
    int w = CAL_POPUP_W, h = CAL_POPUP_H;
    int x = popup_x(w), y = popup_y(h);
    draw_card_bg(x, y, w, h);

    int yy, mm, dd, hh, mi, ss;
    ticks_to_date(timer_ticks, &yy, &mm, &dd, &hh, &mi, &ss);
    char tbuf[16];
    format_time(tbuf, (int)timer_ticks);
    draw_string_centered_bold(x, y + 8, w, tbuf, g_theme.text);

    char dbuf[32];
    int_to_str(yy, dbuf);
    int p = 0;
    while (dbuf[p]) p++;
    if (p < 30) dbuf[p++] = '-';
    int_to_str(mm, dbuf + p);
    p = 0;
    while (dbuf[p]) p++;
    if (p < 30) dbuf[p++] = '-';
    int_to_str(dd, dbuf + p);
    draw_string_centered(x, y + 36, w, dbuf, g_theme.text_secondary);

    const char* dnames = "SMTWTFS";
    int cell_w = (w - 16) / 7;
    int cell_h = 22;
    int gx = x + 8, gy = y + 70;
    for (int i = 0; i < 7; i++) {
        char dn[2];
        dn[0] = dnames[i];
        dn[1] = '\0';
        draw_string_centered(gx + i * cell_w, gy, cell_w, dn, g_theme.text_secondary);
    }

    int dim = days_in_month(yy, mm);
    int first_wd = weekday_of(yy, mm, 1);
    for (int day = 1; day <= dim; day++) {
        int idx = first_wd + day - 1;
        int row = idx / 7;
        int col = idx % 7;
        int cx = gx + col * cell_w;
        int cy = gy + 16 + row * cell_h;
        if (day == dd) {
            fb_fill_circle(cx + cell_w / 2, cy + 8, 9, g_theme.accent);
            char ds[4];
            int_to_str(day, ds);
            draw_string_centered(cx, cy, cell_w, ds, COLOR_TEXT_WHITE);
        } else {
            char ds[4];
            int_to_str(day, ds);
            draw_string_centered(cx, cy, cell_w, ds, g_theme.text);
        }
    }
}

static void systray_render_context_menu(void) {
    if (!s_ctx_visible) return;
    int w = CTX_MENU_W;
    int h = CTX_MENU_ITEMS * CTX_MENU_ITEM_H;
    int x = s_ctx_x, y = s_ctx_y;
    if (x + w > (int)fb_width - 4) x = (int)fb_width - w - 4;
    if (x < 4) x = 4;
    if (y + h > (int)fb_height - TASKBAR_HEIGHT - 4) y = (int)fb_height - TASKBAR_HEIGHT - h - 4;
    if (y < 4) y = 4;
    s_ctx_x = x; s_ctx_y = y;
    fb_fillrect(x, y, w, h, COLOR_MENU_BG);
    fb_draw_rect_outline(x, y, w, h, COLOR_MENU_BORDER);
    const char* labels[CTX_MENU_ITEMS] = { "Open Settings", "Mute / Unmute" };
    for (int i = 0; i < CTX_MENU_ITEMS; i++) {
        int iy = y + i * CTX_MENU_ITEM_H;
        uint32_t bg = (i == s_ctx_sel) ? COLOR_MENU_HOVER : COLOR_MENU_BG;
        fb_fillrect(x + 2, iy + 2, w - 4, CTX_MENU_ITEM_H - 4, bg);
        draw_string(x + 12, iy + 6, labels[i], COLOR_MENU_TEXT);
    }
}

void systray_init(void) {
    s_icons[0].type = SYSTRAY_VOLUME;
    s_icons[1].type = SYSTRAY_NETWORK;
    s_icons[2].type = SYSTRAY_CLOCK;
    for (int i = 0; i < SYSTRAY_ICON_COUNT; i++) {
        s_icons[i].x = 0;
        s_icons[i].y = 0;
        s_icons[i].hover = 0;
        s_icons[i].popup_open = 0;
    }
    s_vol_dragging = 0;
    s_ctx_visible = 0;
    s_vol_cached = audio_get_volume();
}

void systray_render(int taskbar_x, int taskbar_y, int taskbar_w, int taskbar_h) {
    char tb[16];
    format_time(tb, (int)timer_ticks);
    int tw = string_width(tb);
    int tray_right = taskbar_x + taskbar_w - 60 - tw - 8;
    int icon_y = taskbar_y + (taskbar_h - ICON_BOX) / 2;
    for (int i = SYSTRAY_ICON_COUNT - 1; i >= 0; i--) {
        int idx_from_right = SYSTRAY_ICON_COUNT - 1 - i;
        s_icons[i].x = tray_right - (idx_from_right + 1) * ICON_BOX - idx_from_right * ICON_GAP;
        s_icons[i].y = icon_y;
        draw_icon(&s_icons[i]);
    }

    if (s_icons[0].popup_open) render_volume_popup();
    if (s_icons[1].popup_open) render_network_popup();
    if (s_icons[2].popup_open) render_clock_popup();
    systray_render_context_menu();
}

static void start_drag_volume(int mx) {
    int w = VOL_POPUP_W;
    int x = popup_x(w);
    int tx = x + 16, tw = w - 32;
    int v = (mx - tx) * 100 / tw;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    s_vol_cached = v;
    s_vol_dragging = 1;
    audio_set_volume(0, v);
    gui_dirty = 1;
}

static int click_inside_popups(int mx, int my) {
    if (s_icons[0].popup_open) {
        int x = popup_x(VOL_POPUP_W), y = popup_y(VOL_POPUP_H);
        if (mx >= x && mx < x + VOL_POPUP_W && my >= y && my < y + VOL_POPUP_H) {
            int ty = y + 44;
            if (my >= ty - 8 && my < ty + 14) start_drag_volume(mx);
            return 1;
        }
    }
    return 0;
}

static void toggle_popup(systray_icon_t* ic) {
    int was_open = ic->popup_open;
    close_all_popups();
    s_ctx_visible = 0;
    ic->popup_open = !was_open;
    if (ic->type == SYSTRAY_VOLUME && ic->popup_open) s_vol_cached = audio_get_volume();
    gui_dirty = 1;
}

static void show_context_menu(int mx, int my) {
    s_ctx_visible = 1;
    s_ctx_x = mx;
    s_ctx_y = my;
    s_ctx_sel = -1;
    close_all_popups();
    gui_dirty = 1;
}

static int handle_right_click(int mx, int my) {
    if (s_ctx_visible) {
        int w = CTX_MENU_W, h = CTX_MENU_ITEMS * CTX_MENU_ITEM_H;
        int x = s_ctx_x, y = s_ctx_y;
        if (mx >= x && mx < x + w && my >= y && my < y + h) {
            int idx = (my - y) / CTX_MENU_ITEM_H;
            if (idx == 0) {
                gfx_settings_open();
            } else if (idx == 1) {
                int cur = audio_get_volume();
                if (cur > 0) {
                    s_vol_cached = cur;
                    audio_set_volume(0, 0);
                } else {
                    int restore = s_vol_cached > 0 ? s_vol_cached : 75;
                    audio_set_volume(0, restore);
                }
            }
            s_ctx_visible = 0;
            gui_dirty = 1;
            return 1;
        }
        s_ctx_visible = 0;
        gui_dirty = 1;
        return 1;
    }
    systray_icon_t* ic = icon_at(mx, my);
    if (ic) {
        show_context_menu(mx, my);
        return 1;
    }
    return 0;
}

int systray_handle_click(int mx, int my) {
    int right = (mouse_buttons & 0x02) ? 1 : 0;
    if (right) return handle_right_click(mx, my);

    if (s_ctx_visible) {
        s_ctx_visible = 0;
        gui_dirty = 1;
        return 1;
    }

    systray_icon_t* ic = icon_at(mx, my);
    if (ic) {
        toggle_popup(ic);
        return 1;
    }

    int any_open = 0;
    for (int i = 0; i < SYSTRAY_ICON_COUNT; i++) if (s_icons[i].popup_open) any_open = 1;
    if (any_open) {
        if (click_inside_popups(mx, my)) return 1;
        close_all_popups();
        s_vol_dragging = 0;
        gui_dirty = 1;
        return 1;
    }
    return 0;
}

void systray_handle_hover(int mx, int my) {
    if (s_vol_dragging) {
        if (mouse_buttons & 0x01) {
            start_drag_volume(mx);
        } else {
            s_vol_dragging = 0;
        }
        return;
    }

    int changed = 0;
    for (int i = 0; i < SYSTRAY_ICON_COUNT; i++) {
        int h = icon_hit(&s_icons[i], mx, my);
        if (h != s_icons[i].hover) {
            s_icons[i].hover = h;
            changed = 1;
        }
    }

    if (s_ctx_visible) {
        int w = CTX_MENU_W, h = CTX_MENU_ITEMS * CTX_MENU_ITEM_H;
        int x = s_ctx_x, y = s_ctx_y;
        int sel = -1;
        if (mx >= x && mx < x + w && my >= y && my < y + h)
            sel = (my - y) / CTX_MENU_ITEM_H;
        if (sel != s_ctx_sel) { s_ctx_sel = sel; changed = 1; }
    }
    if (changed) gui_dirty = 1;
}
