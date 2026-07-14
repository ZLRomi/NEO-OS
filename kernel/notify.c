#include "gui_internal.h"
#include "notify.h"
#include "easing.h"
#include "idt.h"
#include "theme.h"
#include <stdint.h>

#define NOTIFY_W 320
#define NOTIFY_HEIGHT 80
#define NOTIFY_LIFETIME_TICKS 200
#define NOTIFY_SLIDE_MS 280

static notify_t g_notify[NOTIFY_MAX];
static int g_offset[NOTIFY_MAX];
static int g_anim_id[NOTIFY_MAX];
static int g_dying[NOTIFY_MAX];

static void str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static uint32_t level_color(int level) {
    switch (level) {
        case NOTIFY_LEVEL_SUCCESS: return 0x0000C853;
        case NOTIFY_LEVEL_ERROR:   return 0x00E81123;
        case NOTIFY_LEVEL_WARNING: return 0x00FFB900;
        default: return 0x0000A4EF;
    }
}

static void draw_level_icon(int cx, int cy, int level, uint32_t color) {
    fb_fill_circle(cx, cy, 12, color);
    uint32_t w = COLOR_TEXT_WHITE;
    if (level == NOTIFY_LEVEL_SUCCESS) {
        fb_draw_line(cx - 5, cy, cx - 1, cy + 4, w);
        fb_draw_line(cx - 2, cy + 4, cx + 6, cy - 5, w);
        fb_draw_line(cx - 5, cy + 1, cx - 1, cy + 5, w);
        fb_draw_line(cx - 1, cy + 5, cx + 6, cy - 4, w);
    } else if (level == NOTIFY_LEVEL_ERROR) {
        fb_draw_line(cx - 5, cy - 5, cx + 5, cy + 5, w);
        fb_draw_line(cx - 4, cy - 5, cx + 6, cy + 5, w);
        fb_draw_line(cx + 5, cy - 5, cx - 5, cy + 5, w);
        fb_draw_line(cx + 6, cy - 5, cx - 4, cy + 5, w);
    } else if (level == NOTIFY_LEVEL_WARNING) {
        fb_fillrect(cx - 1, cy - 5, 2, 7, w);
        fb_fillrect(cx - 1, cy + 3, 2, 2, w);
    } else {
        fb_fillrect(cx - 1, cy - 5, 2, 4, w);
        fb_fillrect(cx - 1, cy + 1, 2, 3, w);
    }
}

static void notify_anim_done(int anim_id) {
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (g_anim_id[i] == anim_id) {
            g_anim_id[i] = 0;
            if (g_dying[i]) {
                g_dying[i] = 0;
                g_notify[i].active = 0;
                g_offset[i] = NOTIFY_W;
                gui_dirty = 1;
            }
            return;
        }
    }
}

void notify_init(void) {
    for (int i = 0; i < NOTIFY_MAX; i++) {
        g_notify[i].active = 0;
        g_notify[i].title[0] = '\0';
        g_notify[i].body[0] = '\0';
        g_notify[i].level = NOTIFY_LEVEL_INFO;
        g_notify[i].start_tick = 0;
        g_offset[i] = NOTIFY_W;
        g_anim_id[i] = 0;
        g_dying[i] = 0;
    }
}

void notify_send(const char* title, const char* body, int level) {
    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_notify[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        uint32_t oldest = 0xFFFFFFFFu;
        for (int i = 0; i < NOTIFY_MAX; i++) {
            if (g_dying[i]) continue;
            if (g_notify[i].start_tick < oldest) {
                oldest = g_notify[i].start_tick;
                slot = i;
            }
        }
        if (slot >= 0) {
            if (g_anim_id[slot]) anim_stop(g_anim_id[slot]);
            g_anim_id[slot] = 0;
            g_dying[slot] = 0;
            g_notify[slot].active = 0;
        }
    }
    if (slot < 0) return;

    str_copy(g_notify[slot].title, title, NOTIFY_TITLE_SIZE);
    str_copy(g_notify[slot].body, body, NOTIFY_BODY_SIZE);
    g_notify[slot].level = level;
    g_notify[slot].start_tick = timer_ticks;
    g_notify[slot].active = 1;
    g_dying[slot] = 0;
    g_offset[slot] = NOTIFY_W;
    if (g_anim_id[slot]) anim_stop(g_anim_id[slot]);
    g_anim_id[slot] = anim_start(&g_offset[slot], NOTIFY_SLIDE_MS, NOTIFY_W, 0, notify_anim_done);
    if (g_anim_id[slot] == 0) g_offset[slot] = 0;
    gui_dirty = 1;
}

void notify_update(void) {
    int need_redraw = 0;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_notify[i].active) continue;
        if (!g_dying[i]) {
            uint32_t elapsed = timer_ticks - g_notify[i].start_tick;
            if (elapsed >= NOTIFY_LIFETIME_TICKS) {
                g_dying[i] = 1;
                int from = g_offset[i];
                if (g_anim_id[i]) anim_stop(g_anim_id[i]);
                g_anim_id[i] = anim_start(&g_offset[i], NOTIFY_SLIDE_MS, from, NOTIFY_W, notify_anim_done);
                if (g_anim_id[i] == 0) {
                    g_dying[i] = 0;
                    g_notify[i].active = 0;
                    g_offset[i] = NOTIFY_W;
                }
                gui_dirty = 1;
            }
        }
        if (g_offset[i] != 0 && g_offset[i] != NOTIFY_W) need_redraw = 1;
    }
    if (need_redraw) gui_dirty = 1;
}

void notify_render(void) {
    int base_x = (int)fb_width - NOTIFY_W - 8;
    int bottom_y = (int)fb_height - TASKBAR_HEIGHT - 8;

    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (!g_notify[i].active) continue;
        int di = 0;
        for (int j = 0; j < NOTIFY_MAX; j++) {
            if (i == j || !g_notify[j].active) continue;
            if (g_notify[j].start_tick > g_notify[i].start_tick) di++;
        }
        int x = base_x + g_offset[i];
        int y = bottom_y - NOTIFY_HEIGHT - di * (NOTIFY_HEIGHT + 8);
        if (x >= (int)fb_width) continue;

        fb_fillrect(x + 4, y + 4, NOTIFY_W, NOTIFY_HEIGHT, 0x55000000);
        fb_draw_round_rect(x, y, NOTIFY_W, NOTIFY_HEIGHT, 6, g_theme.bg);
        fb_draw_rect_outline(x, y, NOTIFY_W, NOTIFY_HEIGHT, g_theme.border);

        draw_level_icon(x + 24, y + 32, g_notify[i].level, level_color(g_notify[i].level));

        draw_string_bold(x + 44, y + 10, g_notify[i].title, g_theme.text);

        char tbuf[16];
        uint32_t elapsed = (timer_ticks - g_notify[i].start_tick) / 50u;
        if (elapsed == 0) str_copy(tbuf, "now", 16);
        else {
            char ns[8];
            int_to_str((int)elapsed, ns);
            str_copy(tbuf, ns, 16);
            int p = 0;
            while (tbuf[p]) p++;
            if (p < 14) { tbuf[p++] = 's'; tbuf[p] = '\0'; }
        }
        int tw = string_width(tbuf);
        draw_string(x + NOTIFY_W - 8 - tw, y + 12, tbuf, g_theme.text_secondary);

        draw_string_wrapped(x + 44, y + 36, NOTIFY_W - 52, 24, g_notify[i].body, g_theme.text_secondary);
    }
}
