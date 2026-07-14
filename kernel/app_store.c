#include "app_store.h"
#include "gui_internal.h"
#include "neap.h"
#include "notify.h"
#include "heap.h"
#include "theme.h"
#include <stdint.h>

#define APP_COUNT 5
#define AS_COLS 2
#define AS_PAD 8
#define AS_GAP 8
#define AS_CARD_H 120
#define AS_ICON_SIZE 40
#define AS_BTN_H 24
#define AS_BTN_GAP 8
#define AS_HEADER_H 56

typedef struct {
    const char* name;
    const char* description;
    uint32_t icon_color;
    const char* neap_path;
    int installed;
} app_entry_t;

static app_entry_t apps[APP_COUNT];

static int as_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int as_str_copy(char* dst, int cap, const char* src) {
    int n = 0;
    while (src[n] && n < cap - 1) { dst[n] = src[n]; n++; }
    dst[n] = 0;
    return n;
}

static int as_btn_width(const char* label) {
    return as_strlen(label) * FONT_WIDTH + 16;
}

static int as_card_w(window_t* win) {
    return (win->w - AS_PAD * 2 - AS_GAP) / AS_COLS;
}

static void as_card_rect(window_t* win, int idx, int* cx, int* cy) {
    int col = idx % AS_COLS;
    int row = idx / AS_COLS;
    *cx = win->x + AS_PAD + col * (as_card_w(win) + AS_GAP);
    *cy = win->y + 32 + AS_HEADER_H + row * (AS_CARD_H + AS_GAP);
}

static void as_notify(const char* status, int idx, int level) {
    char body[NOTIFY_BODY_SIZE];
    int j = as_str_copy(body, NOTIFY_BODY_SIZE, status);
    if (j < NOTIFY_BODY_SIZE - 2) {
        body[j++] = ':';
        body[j++] = ' ';
        as_str_copy(body + j, NOTIFY_BODY_SIZE - j, apps[idx].name);
    }
    notify_send("App Store", body, level);
}

static void as_launch(int idx) {
    neap_header_t hdr;
    void* code = 0;
    void* data = 0;
    char manifest[256];
    manifest[0] = 0;
    int rc = neap_load(apps[idx].neap_path, &hdr, &code, &data, manifest, sizeof(manifest));
    if (rc == 0) {
        as_notify("已加载", idx, NOTIFY_LEVEL_SUCCESS);
        if (code) kfree(code);
        if (data) kfree(data);
    } else {
        as_notify("加载失败", idx, NOTIFY_LEVEL_ERROR);
    }
    gui_dirty = 1;
}

void app_store_init(void) {
    apps[0].name = "文本编辑器";
    apps[0].description = "简单的文本编辑器";
    apps[0].icon_color = 0x00783A91;
    apps[0].neap_path = "/apps/editor.neap";
    apps[0].installed = 0;

    apps[1].name = "画图";
    apps[1].description = "绘图应用程序";
    apps[1].icon_color = 0x00E74C3C;
    apps[1].neap_path = "/apps/paint.neap";
    apps[1].installed = 0;

    apps[2].name = "终端";
    apps[2].description = "命令行工具";
    apps[2].icon_color = 0x002C3E50;
    apps[2].neap_path = "/apps/terminal.neap";
    apps[2].installed = 0;

    apps[3].name = "音乐播放器";
    apps[3].description = "播放音频文件";
    apps[3].icon_color = 0x00E91E63;
    apps[3].neap_path = "/apps/music.neap";
    apps[3].installed = 0;

    apps[4].name = "专业计算器";
    apps[4].description = "高级计算器";
    apps[4].icon_color = 0x0000A4EF;
    apps[4].neap_path = "/apps/calc.neap";
    apps[4].installed = 0;
}

void app_store_open(void) {
    window_t* win = window_find_by_type(WINDOW_TYPE_APP_STORE);
    if (!win) return;
    if (win->visible) {
        window_bring_to_front_internal(win);
        gui_dirty = 1;
        return;
    }
    int ex = win->normal_x, ey = win->normal_y, ew = win->normal_w, eh = win->normal_h;
    int sx = win->x + win->w / 2 - 10;
    int sy = win->y + win->h / 2 - 10;
    window_bring_to_front_internal(win);
    window_start_animation(win, 12, sx, sy, 20, 20, ex, ey, ew, eh, 0);
    gui_dirty = 1;
}

static void as_get_buttons(window_t* win, int idx,
                           int* bx1, int* bw1,
                           int* bx2, int* bw2, int* has2) {
    int cx, cy;
    as_card_rect(win, idx, &cx, &cy);
    int cw = as_card_w(win);
    int by = cy + AS_CARD_H - AS_BTN_H - 8;
    *has2 = 0;
    if (apps[idx].installed) {
        int w1 = as_btn_width("打开");
        int w2 = as_btn_width("移除");
        *bw1 = w1; *bw2 = w2;
        *bx2 = cx + cw - 8 - w2;
        *bx1 = *bx2 - AS_BTN_GAP - w1;
        *has2 = 1;
    } else {
        *bw1 = as_btn_width("安装");
        *bw2 = 0;
        *bx1 = cx + cw - 8 - *bw1;
        *bx2 = 0;
    }
    (void)by;
}

void app_store_render(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    (void)h;
    fb_draw_round_rect(x, y, w, h, 8, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int hy = y + 32 + 8;
    draw_string_bold(x + AS_PAD, hy, "应用商店", g_theme.text);
    char sub[48];
    int j = as_str_copy(sub, sizeof(sub), "可用应用：");
    char nbuf[8];
    int_to_str(APP_COUNT, nbuf);
    as_str_copy(sub + j, sizeof(sub) - j, nbuf);
    draw_string(x + AS_PAD, hy + 28, sub, g_theme.text_secondary);

    int cw = as_card_w(win);
    for (int i = 0; i < APP_COUNT; i++) {
        int cx, cy;
        as_card_rect(win, i, &cx, &cy);
        fb_fillrect(cx, cy, cw, AS_CARD_H, g_theme.title_bar_bottom);
        fb_draw_rect_outline(cx, cy, cw, AS_CARD_H, g_theme.border);

        int ix = cx + 8, iy = cy + 8;
        fb_fillrect(ix, iy, AS_ICON_SIZE, AS_ICON_SIZE, apps[i].icon_color);
        fb_draw_round_rect(ix, iy, AS_ICON_SIZE, AS_ICON_SIZE, 6, 0x00FFFFFF);
        char letter[2];
        letter[0] = apps[i].name[0];
        letter[1] = 0;
        draw_string_centered_bold(ix, iy + (AS_ICON_SIZE - FONT_HEIGHT) / 2,
                                  AS_ICON_SIZE, letter, COLOR_TEXT_WHITE);

        int tx = ix + AS_ICON_SIZE + 8;
        draw_string_bold(tx, iy, apps[i].name, g_theme.text);
        draw_string_wrapped(tx, iy + 28, cw - (tx - cx) - 8, 48,
                            apps[i].description, g_theme.text_secondary);

        int bx1, bw1, bx2, bw2, has2;
        as_get_buttons(win, i, &bx1, &bw1, &bx2, &bw2, &has2);
        int by = cy + AS_CARD_H - AS_BTN_H - 8;

        uint32_t b1_bg = apps[i].installed ? g_theme.accent : 0x006699CC;
        fb_fillrect(bx1, by, bw1, AS_BTN_H, b1_bg);
        fb_draw_rect_outline(bx1, by, bw1, AS_BTN_H, g_theme.border);
        const char* b1_label = apps[i].installed ? "打开" : "安装";
        draw_string_centered(bx1, by + (AS_BTN_H - FONT_HEIGHT) / 2, bw1,
                            b1_label, COLOR_TEXT_WHITE);

        if (has2) {
            fb_fillrect(bx2, by, bw2, AS_BTN_H, 0x00CC4444);
            fb_draw_rect_outline(bx2, by, bw2, AS_BTN_H, g_theme.border);
            draw_string_centered(bx2, by + (AS_BTN_H - FONT_HEIGHT) / 2, bw2,
                                 "移除", COLOR_TEXT_WHITE);
        }
    }
}

int app_store_handle_click(window_t* win, int mx, int my) {
    if (!win || !win->visible || win->minimized) return 0;
    if (my < win->y + 32) return 0;

    for (int i = 0; i < APP_COUNT; i++) {
        int cx, cy;
        as_card_rect(win, i, &cx, &cy);
        int cw = as_card_w(win);
        if (mx < cx || mx >= cx + cw || my < cy || my >= cy + AS_CARD_H) continue;

        int bx1, bw1, bx2, bw2, has2;
        as_get_buttons(win, i, &bx1, &bw1, &bx2, &bw2, &has2);
        int by = cy + AS_CARD_H - AS_BTN_H - 8;

        if (mx >= bx1 && mx < bx1 + bw1 && my >= by && my < by + AS_BTN_H) {
            if (apps[i].installed) {
                as_launch(i);
            } else {
                apps[i].installed = 1;
                as_notify("已安装", i, NOTIFY_LEVEL_SUCCESS);
                gui_dirty = 1;
            }
            return 1;
        }

        if (has2 && mx >= bx2 && mx < bx2 + bw2 && my >= by && my < by + AS_BTN_H) {
            apps[i].installed = 0;
            as_notify("已移除", i, NOTIFY_LEVEL_INFO);
            gui_dirty = 1;
            return 1;
        }

        return 1;
    }
    return 0;
}
