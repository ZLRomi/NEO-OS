#include "gui_internal.h"
#include "process.h"
#include "memory.h"
#include "idt.h"
#include "klog.h"
#include "mouse.h"
#include "theme.h"
#include <stdint.h>

static int taskmgr_selected_pid = -1;
#define TASKMGR_HISTORY_SIZE 60
static int cpu_history[TASKMGR_HISTORY_SIZE];
static int mem_history[TASKMGR_HISTORY_SIZE];
static int history_idx = 0;
static int history_count = 0;
static uint32_t last_sample_tick = 0;
static int confirm_visible = 0;
static int confirm_pid = -1;

static const char* state_str(int s) {
    switch (s) {
        case 0: return "就绪";
        case 1: return "运行";
        case 2: return "阻塞";
        case 3: return "终止";
        default: return "?";
    }
}

static uint32_t state_color(int s) {
    switch (s) {
        case 1: return 0x0000AA00;
        case 2: return 0x00CCAA00;
        case 3: return 0x00CC0000;
        default: return 0x00007ACC;
    }
}

static void taskmgr_sample(void) {
    uint32_t now = timer_ticks;
    if (now - last_sample_tick < 50 && last_sample_tick != 0) return;
    last_sample_tick = now;
    process_t* list[MAX_PROCESSES];
    int n = process_get_all(list, MAX_PROCESSES);
    int active = 0;
    for (int i = 0; i < n; i++) {
        if (list[i]->state == 0 || list[i]->state == 1) active++;
    }
    int cpu = active * 15;
    if (cpu > 100) cpu = 100;
    uint32_t total = pmm_get_total_pages();
    uint32_t free_p = pmm_get_free_pages();
    uint32_t used = (total > free_p) ? total - free_p : 0;
    int mem = (total > 0) ? (int)(used * 100 / total) : 0;
    cpu_history[history_idx] = cpu;
    mem_history[history_idx] = mem;
    history_idx = (history_idx + 1) % TASKMGR_HISTORY_SIZE;
    if (history_count < TASKMGR_HISTORY_SIZE) history_count++;
}

static void draw_graph(int gx, int gy, int gw, int gh, int* hist, int count, int idx, uint32_t color, const char* label) {
    fb_draw_round_rect(gx, gy, gw, gh, 4, g_theme.border);
    fb_fillrect(gx + 1, gy + 1, gw - 2, gh - 2, g_theme.bg);
    int y25 = gy + gh - gh / 4;
    int y50 = gy + gh - gh / 2;
    int y75 = gy + gh - 3 * gh / 4;
    fb_draw_line(gx + 1, y25, gx + gw - 2, y25, 0x22AAAAAA);
    fb_draw_line(gx + 1, y50, gx + gw - 2, y50, 0x33AAAAAA);
    fb_draw_line(gx + 1, y75, gx + gw - 2, y75, 0x22AAAAAA);
    if (count > 1) {
        int prev_v = 0;
        for (int i = 0; i < count; i++) {
            int hi = (idx + i) % TASKMGR_HISTORY_SIZE;
            if (i == 0) { prev_v = hist[hi]; continue; }
            int v = hist[hi];
            int px0 = gx + 2 + (i - 1) * (gw - 4) / (TASKMGR_HISTORY_SIZE - 1);
            int py0 = gy + gh - 2 - (prev_v * (gh - 4) / 100);
            int px1 = gx + 2 + i * (gw - 4) / (TASKMGR_HISTORY_SIZE - 1);
            int py1 = gy + gh - 2 - (v * (gh - 4) / 100);
            fb_draw_line(px0, py0, px1, py1, color);
            prev_v = v;
        }
    }
    if (label) draw_string_bold(gx + 4, gy + 2, label, g_theme.text);
}

static void draw_refresh_icon(int cx, int cy, uint32_t color) {
    fb_draw_line(cx, cy - 5, cx + 4, cy - 3, color);
    fb_draw_line(cx + 4, cy - 3, cx + 5, cy, color);
    fb_draw_line(cx + 5, cy, cx + 4, cy + 3, color);
    fb_draw_line(cx + 4, cy + 3, cx, cy + 5, color);
    fb_draw_line(cx, cy + 5, cx - 4, cy + 3, color);
    fb_draw_line(cx - 4, cy + 3, cx - 5, cy, color);
    fb_draw_line(cx, cy - 5, cx + 3, cy - 7, color);
    fb_draw_line(cx, cy - 5, cx + 3, cy - 3, color);
}

static void append_str(char* dst, int* j, int max, const char* s) {
    while (*s && *j < max - 1) dst[(*j)++] = *s++;
}

static void append_dec(char* dst, int* j, int max, int v) {
    char tmp[12];
    int i = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    if (neg && *j < max - 1) dst[(*j)++] = '-';
    while (i > 0 && *j < max - 1) dst[(*j)++] = tmp[--i];
}

void render_taskmgr(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    int mx = mouse_x, my = mouse_y;
    taskmgr_sample();
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int ref_btn_w = 28, ref_btn_h = 24;
    int ref_x = x + w - ref_btn_w - 40;
    int ref_y = y + 4;
    int ref_hover = (mx >= ref_x && mx < ref_x + ref_btn_w && my >= ref_y && my < ref_y + ref_btn_h);
    fb_fillrect(ref_x, ref_y, ref_btn_w, ref_btn_h, ref_hover ? g_theme.border : g_theme.bg);
    draw_refresh_icon(ref_x + ref_btn_w / 2, ref_y + ref_btn_h / 2, g_theme.text);

    int graph_y = y + 32;
    int graph_h = 64;
    int graph_w = (w - 24) / 2;
    int cpu_gx = x + 8;
    int mem_gx = cpu_gx + graph_w + 8;
    draw_graph(cpu_gx, graph_y, graph_w, graph_h, cpu_history, history_count, history_idx, g_theme.accent, "CPU");
    draw_graph(mem_gx, graph_y, graph_w, graph_h, mem_history, history_count, history_idx, 0x0000AA00, "内存");

    int header_y = graph_y + graph_h + 4;
    draw_string_bold(x + 12, header_y, "进程号", g_theme.text);
    draw_string_bold(x + 80, header_y, "名称", g_theme.text);
    draw_string_bold(x + 270, header_y, "状态", g_theme.text);
    draw_string_bold(x + 450, header_y, "优先级", g_theme.text);
    fb_fillrect(x + 8, header_y + 24, w - 16, 1, g_theme.border);

    process_t* list[MAX_PROCESSES];
    int n = process_get_all(list, MAX_PROCESSES);

    int row_h = 28;
    int list_y = header_y + 28;
    int footer_h = 40;
    int max_rows = (h - (list_y - y) - footer_h) / row_h;
    if (max_rows < 1) max_rows = 1;

    for (int i = 0; i < n && i < max_rows; i++) {
        process_t* p = list[i];
        int ry = list_y + i * row_h;
        if (p->pid == taskmgr_selected_pid) {
            fb_fillrect(x + 8, ry - 2, w - 16, row_h, 0x22CCCCCC);
        }
        fb_fill_circle(x + 20, ry + 10, 4, state_color(p->state));
        char buf[16];
        int j = 0;
        append_dec(buf, &j, 16, p->pid);
        buf[j] = '\0';
        draw_string(x + 32, ry, buf, g_theme.text);
        draw_string(x + 80, ry, p->name, g_theme.text);
        draw_string(x + 270, ry, state_str(p->state), g_theme.text);
        j = 0;
        append_dec(buf, &j, 16, p->priority);
        buf[j] = '\0';
        draw_string(x + 450, ry, buf, g_theme.text);
    }

    int foot_y = y + h - footer_h;
    fb_fillrect(x + 8, foot_y, w - 16, 1, g_theme.border);

    char mbuf[96];
    int j = 0;
    uint32_t total = pmm_get_total_pages();
    uint32_t free_p = pmm_get_free_pages();
    uint32_t used = (total > free_p) ? total - free_p : 0;
    append_str(mbuf, &j, 96, "进程数：");
    append_dec(mbuf, &j, 96, n);
    append_str(mbuf, &j, 96, "  内存：");
    append_dec(mbuf, &j, 96, (int)used);
    append_str(mbuf, &j, 96, "/");
    append_dec(mbuf, &j, 96, (int)total);
    mbuf[j] = '\0';
    draw_string(x + 12, foot_y + 8, mbuf, g_theme.text_secondary);

    int btn_w = 96, btn_h = 28;
    int btn_x = x + w - btn_w - 12;
    int btn_y = foot_y + 6;
    int btn_hover = (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h);
    uint32_t btn_color = (taskmgr_selected_pid > 0) ? (btn_hover ? 0x00CC4444 : 0x00BB3333) : 0x00999999;
    fb_fillrect(btn_x, btn_y, btn_w, btn_h, btn_color);
    fb_draw_round_rect(btn_x, btn_y, btn_w, btn_h, 4, g_theme.border);
    draw_string_centered(btn_x, btn_y + 4, btn_w, "结束任务", COLOR_TEXT_WHITE);

    if (confirm_visible) {
        int dw = 260, dh = 120;
        int dx = x + (w - dw) / 2;
        int dy = y + (h - dh) / 2;
        fb_fillrect(dx, dy, dw, dh, g_theme.bg);
        fb_draw_round_rect(dx, dy, dw, dh, 6, g_theme.accent);
        draw_string_bold(dx + 16, dy + 12, "结束任务？", g_theme.text);
        char cbuf[64];
        int cj = 0;
        append_str(cbuf, &cj, 64, "终止进程 ");
        append_dec(cbuf, &cj, 64, confirm_pid);
        append_str(cbuf, &cj, 64, "？");
        cbuf[cj] = '\0';
        draw_string_wrapped(dx + 16, dy + 36, dw - 32, 40, cbuf, g_theme.text_secondary);

        int yes_w = 80, no_w = 80;
        int yes_x = dx + dw - yes_w - no_w - 24;
        int no_x = dx + dw - no_w - 12;
        int cy = dy + dh - 32;
        int yes_hover = (mx >= yes_x && mx < yes_x + yes_w && my >= cy && my < cy + 24);
        int no_hover = (mx >= no_x && mx < no_x + no_w && my >= cy && my < cy + 24);
        fb_fillrect(yes_x, cy, yes_w, 24, yes_hover ? 0x00CC4444 : 0x00AA3333);
        fb_draw_round_rect(yes_x, cy, yes_w, 24, 4, g_theme.border);
        draw_string_centered(yes_x, cy + 4, yes_w, "是", COLOR_TEXT_WHITE);
        fb_fillrect(no_x, cy, no_w, 24, no_hover ? g_theme.border : g_theme.bg);
        fb_draw_round_rect(no_x, cy, no_w, 24, 4, g_theme.border);
        draw_string_centered(no_x, cy + 4, no_w, "否", g_theme.text);
    }
}

void taskmgr_handle_click(window_t* win, int mx, int my) {
    if (!win) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;

    if (confirm_visible) {
        int dw = 260, dh = 120;
        int dx = x + (w - dw) / 2;
        int dy = y + (h - dh) / 2;
        int yes_w = 80, no_w = 80;
        int yes_x = dx + dw - yes_w - no_w - 24;
        int no_x = dx + dw - no_w - 12;
        int cy = dy + dh - 32;
        if (mx >= yes_x && mx < yes_x + yes_w && my >= cy && my < cy + 24) {
            if (confirm_pid > 0) {
                process_t* cur = process_get_current();
                if (cur && cur->pid == confirm_pid) {
                    klog_write_level(KLOG_WARN, "taskmgr: refusing to kill current pid=%d\n", confirm_pid);
                } else {
                    klog_write_level(KLOG_INFO, "taskmgr: end task pid=%d\n", confirm_pid);
                    process_exit(confirm_pid);
                    taskmgr_selected_pid = -1;
                }
            }
            confirm_visible = 0;
            confirm_pid = -1;
            gui_dirty = 1;
            return;
        }
        if (mx >= no_x && mx < no_x + no_w && my >= cy && my < cy + 24) {
            confirm_visible = 0;
            confirm_pid = -1;
            gui_dirty = 1;
            return;
        }
        return;
    }

    int ref_btn_w = 28, ref_btn_h = 24;
    int ref_x = x + w - ref_btn_w - 40;
    int ref_y = y + 4;
    if (mx >= ref_x && mx < ref_x + ref_btn_w && my >= ref_y && my < ref_y + ref_btn_h) {
        last_sample_tick = 0;
        history_count = 0;
        history_idx = 0;
        gui_dirty = 1;
        return;
    }

    int graph_y = y + 32;
    int graph_h = 64;
    int header_y = graph_y + graph_h + 4;
    int row_h = 28;
    int list_y = header_y + 28;
    int footer_h = 40;
    int max_rows = (h - (list_y - y) - footer_h) / row_h;
    if (max_rows < 1) max_rows = 1;

    int btn_w = 96, btn_h = 28;
    int btn_x = x + w - btn_w - 12;
    int btn_y = y + h - footer_h + 6;
    if (mx >= btn_x && mx < btn_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
        if (taskmgr_selected_pid > 0) {
            confirm_visible = 1;
            confirm_pid = taskmgr_selected_pid;
            gui_dirty = 1;
        }
        return;
    }

    if (mx >= x + 8 && mx < x + w - 8 && my >= list_y - 2 && my < list_y + max_rows * row_h) {
        int idx = (my - (list_y - 2)) / row_h;
        if (idx >= 0 && idx < max_rows) {
            process_t* list[MAX_PROCESSES];
            int n = process_get_all(list, MAX_PROCESSES);
            if (idx < n) {
                int pid = list[idx]->pid;
                taskmgr_selected_pid = (taskmgr_selected_pid == pid) ? -1 : pid;
                gui_dirty = 1;
            }
        }
    }
}
