#include "download.h"
#include "gui_internal.h"
#include "http.h"
#include "fat32.h"
#include "serial.h"
#include "mouse.h"
#include "theme.h"
#include "idt.h"
#include <stdint.h>

dl_manager_t dl_mgr;

static int str_len(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = '\0';
}

static int is_sep(char c) {
    return c == '/' || c == '\\' || c == ':' || c == '?' || c == '#' || c == '&';
}

static void extract_filename(const char* url, char* out, int max) {
    out[0] = '\0';
    int last = -1;
    int i = 0;
    while (url[i]) {
        if (is_sep(url[i])) last = i;
        i++;
    }
    int start = last + 1;
    int j = 0;
    while (url[start] && url[start] != '?' && url[start] != '#' && j < max - 1) {
        out[j++] = url[start++];
    }
    out[j] = '\0';
    if (j == 0) {
        str_copy(out, "download.bin", max);
    }
}

static int sanitize_filename(char* name) {
    int i = 0;
    while (name[i]) {
        char c = name[i];
        if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '*' ||
            c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            name[i] = '_';
        }
        i++;
        if (i >= DL_NAME_SIZE - 1) break;
    }
    name[i] = '\0';
    return i;
}

void download_init(void) {
    for (int i = 0; i < DL_MAX_TASKS; i++) {
        dl_mgr.tasks[i].used = 0;
        dl_mgr.tasks[i].state = DL_STATE_IDLE;
        dl_mgr.tasks[i].url[0] = '\0';
        dl_mgr.tasks[i].filename[0] = '\0';
        dl_mgr.tasks[i].save_path[0] = '\0';
        dl_mgr.tasks[i].total_bytes = 0;
        dl_mgr.tasks[i].received_bytes = 0;
        dl_mgr.tasks[i].progress = 0;
        dl_mgr.tasks[i].prev_bytes = 0;
        dl_mgr.tasks[i].speed_kbs = 0;
        dl_mgr.tasks[i].last_speed_tick = 0;
    }
    dl_mgr.count = 0;
    dl_mgr.visible = 0;
    dl_mgr.selected = 0;
    dl_mgr.ctx_menu_visible = 0;
    dl_mgr.ctx_menu_x = 0;
    dl_mgr.ctx_menu_y = 0;
    dl_mgr.ctx_menu_idx = -1;
}

static int alloc_task(void) {
    for (int i = 0; i < DL_MAX_TASKS; i++) {
        if (!dl_mgr.tasks[i].used) return i;
    }
    return -1;
}

typedef struct {
    int task_idx;
} dl_ctx_t;

static int progress_cb(int received, int total, void* ctx) {
    dl_ctx_t* c = (dl_ctx_t*)ctx;
    if (c->task_idx < 0 || c->task_idx >= DL_MAX_TASKS) return -1;
    dl_task_t* t = &dl_mgr.tasks[c->task_idx];
    if (!t->used) return -1;
    if (t->state == DL_STATE_CANCELED) return -1;
    t->received_bytes = received;
    t->total_bytes = total;
    if (total > 0) {
        t->progress = received * 100 / total;
    } else {
        t->progress = 0;
    }
    uint32_t now = timer_ticks;
    int dt = (int)(now - (uint32_t)t->last_speed_tick);
    if (dt >= 50) {
        int diff = received - t->prev_bytes;
        int kbs = diff * 10 / 1024 / dt;
        if (kbs < 0) kbs = 0;
        t->speed_kbs = kbs;
        t->prev_bytes = received;
        t->last_speed_tick = (int)now;
    }
    gui_dirty = 1;
    return 0;
}

int download_start(const char* url, const char* save_dir) {
    if (!url || !save_dir) return -1;
    int idx = alloc_task();
    if (idx < 0) return -1;

    dl_task_t* t = &dl_mgr.tasks[idx];
    t->used = 1;
    t->state = DL_STATE_RUNNING;
    str_copy(t->url, url, DL_URL_SIZE);
    extract_filename(url, t->filename, DL_NAME_SIZE);
    sanitize_filename(t->filename);

    int dir_len = str_len(save_dir);
    int fname_len = str_len(t->filename);
    int pos = 0;
    if (save_dir[0] != '/' && dir_len + 1 < DL_PATH_SIZE) {
        t->save_path[pos++] = '/';
    }
    for (int i = 0; i < dir_len && pos < DL_PATH_SIZE - 1; i++) {
        t->save_path[pos++] = save_dir[i];
    }
    if (pos > 0 && t->save_path[pos - 1] != '/' && pos < DL_PATH_SIZE - 1) {
        t->save_path[pos++] = '/';
    }
    for (int i = 0; i < fname_len && pos < DL_PATH_SIZE - 1; i++) {
        t->save_path[pos++] = t->filename[i];
    }
    t->save_path[pos] = '\0';

    t->received_bytes = 0;
    t->total_bytes = 0;
    t->progress = 0;

    serial_write("DL: starting ");
    serial_write(url);
    serial_write(" -> ");
    serial_write(t->save_path);
    serial_write("\n");

    dl_ctx_t ctx;
    ctx.task_idx = idx;
    int r = http_download_file(url, t->save_path, progress_cb, &ctx);
    if (r > 0) {
        t->state = DL_STATE_DONE;
        t->progress = 100;
        serial_write("DL: done ");
        char buf[16];
        int_to_str(r, buf);
        serial_write(buf);
        serial_write(" bytes\n");
    } else if (r == -2 || t->state == DL_STATE_CANCELED) {
        t->state = DL_STATE_CANCELED;
        serial_write("DL: canceled\n");
    } else {
        t->state = DL_STATE_FAILED;
        serial_write("DL: failed\n");
    }
    gui_dirty = 1;
    return r;
}

void download_cancel(int idx) {
    if (idx < 0 || idx >= DL_MAX_TASKS) return;
    dl_task_t* t = &dl_mgr.tasks[idx];
    if (!t->used) return;
    if (t->state == DL_STATE_RUNNING) {
        t->state = DL_STATE_CANCELED;
    }
    gui_dirty = 1;
}

void download_remove(int idx) {
    if (idx < 0 || idx >= DL_MAX_TASKS) return;
    dl_task_t* t = &dl_mgr.tasks[idx];
    if (!t->used) return;
    if (t->state == DL_STATE_RUNNING) return;
    t->used = 0;
    t->state = DL_STATE_IDLE;
    gui_dirty = 1;
}

void downloads_open(void) {
    dl_mgr.visible = 1;
    gui_dirty = 1;
}

static int dl_ext_is(const char* name, const char* ext) {
    int nl = str_len(name);
    int el = str_len(ext);
    if (nl < el) return 0;
    for (int i = 0; i < el; i++) {
        char a = name[nl - el + i];
        char b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int dl_file_type(const char* name) {
    if (dl_ext_is(name, ".txt") || dl_ext_is(name, ".doc") || dl_ext_is(name, ".pdf")) return 1;
    if (dl_ext_is(name, ".zip") || dl_ext_is(name, ".rar") || dl_ext_is(name, ".gz")) return 2;
    if (dl_ext_is(name, ".mp3") || dl_ext_is(name, ".wav") || dl_ext_is(name, ".ogg")) return 3;
    if (dl_ext_is(name, ".mp4") || dl_ext_is(name, ".avi") || dl_ext_is(name, ".mkv")) return 4;
    if (dl_ext_is(name, ".exe") || dl_ext_is(name, ".bin")) return 5;
    if (dl_ext_is(name, ".png") || dl_ext_is(name, ".jpg") || dl_ext_is(name, ".bmp")) return 6;
    return 0;
}

static void dl_draw_file_icon(int cx, int cy, int type, uint32_t color) {
    fb_draw_rect_outline(cx - 7, cy - 8, 14, 16, color);
    fb_draw_line(cx - 7, cy - 8, cx + 7, cy - 8, color);
    if (type == 1) {
        fb_draw_line(cx - 4, cy - 3, cx + 4, cy - 3, color);
        fb_draw_line(cx - 4, cy, cx + 4, cy, color);
        fb_draw_line(cx - 4, cy + 3, cx + 4, cy + 3, color);
    } else if (type == 2) {
        fb_draw_line(cx, cy - 8, cx, cy + 8, color);
        fb_draw_line(cx - 3, cy - 5, cx + 3, cy - 5, color);
        fb_draw_line(cx - 3, cy + 5, cx + 3, cy + 5, color);
    } else if (type == 3) {
        fb_draw_circle(cx, cy, 5, color);
        fb_draw_line(cx, cy - 5, cx, cy - 8, color);
        fb_draw_line(cx, cy - 8, cx + 3, cy - 7, color);
    } else if (type == 4) {
        fb_draw_line(cx - 3, cy - 3, cx + 4, cy, color);
        fb_draw_line(cx + 4, cy, cx - 3, cy + 3, color);
        fb_draw_line(cx - 3, cy - 3, cx - 3, cy + 3, color);
    } else if (type == 5) {
        fb_draw_line(cx - 4, cy - 4, cx + 4, cy - 4, color);
        fb_draw_line(cx - 4, cy + 4, cx + 4, cy + 4, color);
        fb_draw_line(cx - 4, cy - 4, cx - 4, cy + 4, color);
        fb_draw_line(cx + 4, cy - 4, cx + 4, cy + 4, color);
    } else if (type == 6) {
        fb_fill_circle(cx, cy, 4, color);
        fb_draw_line(cx - 6, cy - 2, cx - 2, cy + 2, color);
    } else {
        fb_draw_line(cx - 4, cy - 3, cx + 4, cy - 3, color);
        fb_draw_line(cx - 4, cy + 3, cx + 4, cy + 3, color);
    }
}

static void dl_draw_toolbar_icon_pause(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 3, cy - 5, cx - 3, cy + 5, color);
    fb_draw_line(cx - 2, cy - 5, cx - 2, cy + 5, color);
    fb_draw_line(cx + 2, cy - 5, cx + 2, cy + 5, color);
    fb_draw_line(cx + 3, cy - 5, cx + 3, cy + 5, color);
}

static void dl_draw_toolbar_icon_clear(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 5, cy - 5, cx + 5, cy + 5, color);
    fb_draw_line(cx + 5, cy - 5, cx - 5, cy + 5, color);
}

static void dl_draw_toolbar_icon_add(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 5, cy, cx + 5, cy, color);
    fb_draw_line(cx, cy - 5, cx, cy + 5, color);
}

static void dl_format_size(int bytes, char* out) {
    if (bytes < 1024) {
        int_to_str(bytes, out);
        int l = str_len(out);
        out[l] = 'B';
        out[l + 1] = '\0';
    } else if (bytes < 1024 * 1024) {
        int kb = bytes / 1024;
        int_to_str(kb, out);
        int l = str_len(out);
        out[l] = 'K';
        out[l + 1] = 'B';
        out[l + 2] = '\0';
    } else {
        int mb = bytes / (1024 * 1024);
        int_to_str(mb, out);
        int l = str_len(out);
        out[l] = 'M';
        out[l + 1] = 'B';
        out[l + 2] = '\0';
    }
}

void render_downloads(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    int mx = mouse_x, my = mouse_y;
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    int toolbar_y = y + 32;
    int toolbar_h = 32;
    int btn_w = 96;
    int btn_h = 24;
    int pause_x = x + 8;
    int clear_x = pause_x + btn_w + 8;
    int add_x = clear_x + btn_w + 8;

    int tb_x[3] = { pause_x, clear_x, add_x };
    const char* tb_labels[3] = { "全部暂停", "全部清除", "添加链接" };
    for (int i = 0; i < 3; i++) {
        int bx = tb_x[i];
        int by = toolbar_y + (toolbar_h - btn_h) / 2;
        int hover = (mx >= bx && mx < bx + btn_w && my >= by && my < by + btn_h);
        fb_fillrect(bx, by, btn_w, btn_h, hover ? g_theme.border : g_theme.bg);
        fb_draw_round_rect(bx, by, btn_w, btn_h, 4, g_theme.border);
        int icx = bx + 12;
        int icy = by + btn_h / 2;
        if (i == 0) dl_draw_toolbar_icon_pause(icx, icy, g_theme.text);
        else if (i == 1) dl_draw_toolbar_icon_clear(icx, icy, g_theme.text);
        else dl_draw_toolbar_icon_add(icx, icy, g_theme.text);
        draw_string(bx + 24, by + 4, tb_labels[i], g_theme.text);
    }

    int list_y = toolbar_y + toolbar_h + 4;
    int list_h = h - (list_y - y) - 8;
    if (list_h < 0) list_h = 0;

    fb_fillrect(x + 4, list_y, w - 8, list_h, g_theme.bg);
    int row_h = 56;
    int rows = list_h / row_h;
    if (rows < 0) rows = 0;
    int count = 0;
    for (int i = 0; i < DL_MAX_TASKS && count < rows; i++) {
        dl_task_t* t = &dl_mgr.tasks[i];
        if (!t->used) continue;
        int ry = list_y + count * row_h;
        int row_hover = (mx >= x + 4 && mx < x + w - 4 && my >= ry && my < ry + row_h - 2);
        if (i == dl_mgr.selected) {
            fb_fillrect(x + 6, ry, w - 12, row_h - 2, 0x22CCCCCC);
        } else if (row_hover) {
            fb_fillrect(x + 6, ry, w - 12, row_h - 2, 0x11CCCCCC);
        }
        fb_draw_line(x + 6, ry + row_h - 2, x + w - 6, ry + row_h - 2, g_theme.border);

        int icon_cx = x + 20;
        int icon_cy = ry + 18;
        uint32_t icon_color = g_theme.accent;
        if (t->state == DL_STATE_DONE) icon_color = 0x00008800;
        else if (t->state == DL_STATE_FAILED) icon_color = 0x00CC0000;
        else if (t->state == DL_STATE_CANCELED) icon_color = g_theme.text_secondary;
        dl_draw_file_icon(icon_cx, icon_cy, dl_file_type(t->filename), icon_color);

        draw_string(x + 36, ry + 4, t->filename, g_theme.text);

        char size_buf[24];
        if (t->total_bytes > 0) {
            char recv_buf[16];
            char total_buf[16];
            dl_format_size(t->received_bytes, recv_buf);
            dl_format_size(t->total_bytes, total_buf);
            int ri = 0, ti = 0, si = 0;
            while (recv_buf[ri] && si < 20) size_buf[si++] = recv_buf[ri++];
            size_buf[si++] = '/';
            while (total_buf[ti] && si < 22) size_buf[si++] = total_buf[ti++];
            size_buf[si] = '\0';
        } else {
            dl_format_size(t->received_bytes, size_buf);
        }
        draw_string(x + 36, ry + 26, size_buf, g_theme.text_secondary);

        int bar_x = x + w - 200;
        int bar_y = ry + 24;
        int bar_w = 160;
        int bar_h = 10;
        fb_fillrect(bar_x, bar_y, bar_w, bar_h, g_theme.border);
        int fill_w = bar_w * t->progress / 100;
        if (fill_w > 0) {
            uint32_t c1 = g_theme.accent;
            uint32_t c2 = g_theme.accent_hover;
            int r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
            int r2 = (c2 >> 16) & 0xFF, g2 = (c2 >> 8) & 0xFF, b2 = c2 & 0xFF;
            for (int px = 0; px < fill_w; px++) {
                int fr = r1 + (r2 - r1) * px / bar_w;
                int fg = g1 + (g2 - g1) * px / bar_w;
                int fb = b1 + (b2 - b1) * px / bar_w;
                fb_fillrect(bar_x + px, bar_y, 1, bar_h, ((uint32_t)fr << 16) | ((uint32_t)fg << 8) | (uint32_t)fb);
            }
        }
        fb_draw_round_rect(bar_x, bar_y, bar_w, bar_h, 3, g_theme.border);

        char pct[8];
        int_to_str(t->progress, pct);
        int pl = str_len(pct);
        pct[pl] = '%';
        pct[pl + 1] = '\0';
        int pct_x = bar_x + bar_w + 8;
        draw_string(pct_x, bar_y - 2, pct, g_theme.text);

        if (t->state == DL_STATE_RUNNING && t->speed_kbs > 0) {
            char spd[16];
            int_to_str(t->speed_kbs, spd);
            int sl = str_len(spd);
            spd[sl] = 'K';
            spd[sl + 1] = 'B';
            spd[sl + 2] = '/';
            spd[sl + 3] = 's';
            spd[sl + 4] = '\0';
            draw_string(x + 36, ry + 42, spd, g_theme.text_secondary);
        } else {
            const char* status_str = "";
            switch (t->state) {
                case DL_STATE_RUNNING: status_str = "下载中…"; break;
                case DL_STATE_DONE: status_str = "已完成"; break;
                case DL_STATE_FAILED: status_str = "失败"; break;
                case DL_STATE_CANCELED: status_str = "已取消"; break;
                default: status_str = "空闲"; break;
            }
            draw_string(x + 36, ry + 42, status_str, g_theme.text_secondary);
        }

        count++;
    }

    if (count == 0) {
        draw_string_centered(x, list_y + 20, w, "没有下载任务", g_theme.text_secondary);
    }

    if (dl_mgr.ctx_menu_visible) {
        int cmx = dl_mgr.ctx_menu_x;
        int cmy = dl_mgr.ctx_menu_y;
        int cmw = 140;
        int cmh = 72;
        if (cmx + cmw > x + w - 4) cmx = x + w - 4 - cmw;
        if (cmy + cmh > y + h - 4) cmy = y + h - 4 - cmh;
        fb_fillrect(cmx, cmy, cmw, cmh, g_theme.bg);
        fb_draw_round_rect(cmx, cmy, cmw, cmh, 4, g_theme.border);
        const char* items[3] = { "打开目录", "取消", "移除" };
        for (int i = 0; i < 3; i++) {
            int iy = cmy + i * 24;
            int ihover = (mx >= cmx && mx < cmx + cmw && my >= iy && my < iy + 24);
            if (ihover) fb_fillrect(cmx + 1, iy + 1, cmw - 2, 22, g_theme.border);
            draw_string(cmx + 8, iy + 5, items[i], g_theme.text);
        }
    }
}

void downloads_handle_click(window_t* win, int mx, int my) {
    if (!win) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;

    if (dl_mgr.ctx_menu_visible) {
        int cmx = dl_mgr.ctx_menu_x;
        int cmy = dl_mgr.ctx_menu_y;
        int cmw = 140;
        int cmh = 72;
        if (cmx + cmw > x + w - 4) cmx = x + w - 4 - cmw;
        if (cmy + cmh > y + h - 4) cmy = y + h - 4 - cmh;
        int idx = dl_mgr.ctx_menu_idx;
        if (mx >= cmx && mx < cmx + cmw && my >= cmy && my < cmy + cmh) {
            int item = (my - cmy) / 24;
            if (item == 0) {
                dl_mgr.ctx_menu_visible = 0;
                gui_dirty = 1;
            } else if (item == 1) {
                download_cancel(idx);
                dl_mgr.ctx_menu_visible = 0;
                gui_dirty = 1;
            } else if (item == 2) {
                download_remove(idx);
                dl_mgr.ctx_menu_visible = 0;
                gui_dirty = 1;
            }
            return;
        }
        dl_mgr.ctx_menu_visible = 0;
        gui_dirty = 1;
        return;
    }

    int toolbar_y = y + 32;
    int toolbar_h = 32;
    int btn_w = 96;
    int btn_h = 24;
    int pause_x = x + 8;
    int clear_x = pause_x + btn_w + 8;
    int add_x = clear_x + btn_w + 8;
    int by = toolbar_y + (toolbar_h - btn_h) / 2;
    if (my >= by && my < by + btn_h) {
        if (mx >= pause_x && mx < pause_x + btn_w) {
            for (int i = 0; i < DL_MAX_TASKS; i++) {
                if (dl_mgr.tasks[i].used && dl_mgr.tasks[i].state == DL_STATE_RUNNING) {
                    dl_mgr.tasks[i].state = DL_STATE_CANCELED;
                }
            }
            gui_dirty = 1;
            return;
        }
        if (mx >= clear_x && mx < clear_x + btn_w) {
            for (int i = 0; i < DL_MAX_TASKS; i++) {
                if (dl_mgr.tasks[i].used && dl_mgr.tasks[i].state != DL_STATE_RUNNING) {
                    dl_mgr.tasks[i].used = 0;
                    dl_mgr.tasks[i].state = DL_STATE_IDLE;
                }
            }
            gui_dirty = 1;
            return;
        }
        if (mx >= add_x && mx < add_x + btn_w) {
            gui_dirty = 1;
            return;
        }
    }

    int list_y = toolbar_y + toolbar_h + 4;
    int list_h = h - (list_y - y) - 8;
    int row_h = 56;
    int rows = list_h / row_h;

    if (mx >= x + 4 && mx < x + w - 4 &&
        my >= list_y && my < list_y + rows * row_h) {
        int rel = my - list_y;
        int clicked_row = rel / row_h;
        int count = 0;
        int clicked_idx = -1;
        for (int i = 0; i < DL_MAX_TASKS; i++) {
            if (!dl_mgr.tasks[i].used) continue;
            if (count == clicked_row) {
                clicked_idx = i;
                break;
            }
            count++;
        }
        if (clicked_idx >= 0) {
            dl_mgr.selected = clicked_idx;
            dl_mgr.ctx_menu_visible = 1;
            dl_mgr.ctx_menu_x = mx;
            dl_mgr.ctx_menu_y = my;
            dl_mgr.ctx_menu_idx = clicked_idx;
            gui_dirty = 1;
        }
    }
}
