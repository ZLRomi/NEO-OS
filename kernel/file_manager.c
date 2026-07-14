#include "gui_internal.h"
#include "file_manager.h"
#include "fat32.h"
#include "theme.h"
#include "player.h"
#include "cmd.h"
#include "mouse.h"
#include "idt.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define FM_MAX_ENTRIES 64
#define FM_STACK_DEPTH 8
#define FM_NAME_LEN 16
#define FM_ROW_H 22
#define FM_HEADER_H 24
#define FM_TOOLBAR_H 36
#define FM_STATUS_H 22
#define FM_BTN_W 28
#define FM_BTN_H 22
#define FM_ADDR_X (FM_BTN_W * 3 + 24 + 16)
#define FM_COL_NAME_X 36
#define FM_COL_SIZE_X 360
#define FM_COL_DATE_X 460

typedef enum {
    FM_DIALOG_NONE = 0,
    FM_DIALOG_DELETE,
    FM_DIALOG_RENAME,
    FM_DIALOG_NEW_FOLDER
} fm_dialog_t;

typedef struct {
    char cwd[MAX_PATH];
    char back_stack[FM_STACK_DEPTH][MAX_PATH];
    int back_count;
    char forward_stack[FM_STACK_DEPTH][MAX_PATH];
    int forward_count;

    char names[FM_MAX_ENTRIES][FM_NAME_LEN];
    int isdir[FM_MAX_ENTRIES];
    uint32_t sizes[FM_MAX_ENTRIES];
    int count;

    int selected;
    int hovered;
    int last_click_idx;
    uint32_t last_click_ticks;

    int menu_visible;
    int menu_x, menu_y;
    int menu_target;

    fm_dialog_t dialog;
    int dialog_target;
    char input_buf[FM_NAME_LEN];
    int input_len;

    int rename_active;
} fm_state_t;

static fm_state_t fm;

static int my_strlen(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static int my_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static void my_strcpy(char* d, const char* s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int endswith_ci(const char* s, const char* suffix) {
    int sl = my_strlen(s);
    int su = my_strlen(suffix);
    if (sl < su) return 0;
    for (int i = 0; i < su; i++) {
        char a = s[sl - su + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static void path_join(const char* dir, const char* name, char* out) {
    int i = 0;
    while (dir[i] && i < MAX_PATH - 1) { out[i] = dir[i]; i++; }
    if (i == 0 || out[i - 1] != '/') {
        if (i < MAX_PATH - 1) out[i++] = '/';
    }
    int j = 0;
    while (name[j] && i < MAX_PATH - 1) out[i++] = name[j++];
    out[i] = 0;
}

static void path_parent(const char* path, char* out) {
    int i = 0;
    int last_slash = -1;
    while (path[i]) {
        if (path[i] == '/') last_slash = i;
        i++;
    }
    if (last_slash <= 0) {
        out[0] = '/';
        out[1] = 0;
        return;
    }
    for (int j = 0; j < last_slash; j++) out[j] = path[j];
    out[last_slash] = 0;
    if (out[0] == 0) { out[0] = '/'; out[1] = 0; }
}

static void fm_refresh(void) {
    fm.count = 0;
    fm.selected = -1;
    if (!fat32_is_mounted()) return;
    int dh = fat32_opendir(fm.cwd);
    if (dh < 0) return;
    char name[FM_NAME_LEN];
    int isdir;
    uint32_t size;
    while (fm.count < FM_MAX_ENTRIES && fat32_readdir(dh, name, &isdir, &size)) {
        if (my_streq(name, ".") || my_streq(name, "..")) continue;
        my_strcpy(fm.names[fm.count], name, FM_NAME_LEN);
        fm.isdir[fm.count] = isdir;
        fm.sizes[fm.count] = size;
        fm.count++;
    }
    fat32_close(dh);
    int n = fm.count;
    for (int i = 0; i < n - 1; i++) {
        for (int j = i + 1; j < n; j++) {
            int swap = 0;
            if (fm.isdir[i] != fm.isdir[j]) {
                swap = fm.isdir[j];
            } else {
                int k = 0;
                while (fm.names[i][k] && fm.names[j][k] && fm.names[i][k] == fm.names[j][k]) k++;
                swap = (fm.names[i][k] > fm.names[j][k]);
            }
            if (swap) {
                char tmpn[FM_NAME_LEN];
                my_strcpy(tmpn, fm.names[i], FM_NAME_LEN);
                my_strcpy(fm.names[i], fm.names[j], FM_NAME_LEN);
                my_strcpy(fm.names[j], tmpn, FM_NAME_LEN);
                int td = fm.isdir[i]; fm.isdir[i] = fm.isdir[j]; fm.isdir[j] = td;
                uint32_t ts = fm.sizes[i]; fm.sizes[i] = fm.sizes[j]; fm.sizes[j] = ts;
            }
        }
    }
}

void file_manager_init(void) {
    fm.cwd[0] = '/';
    fm.cwd[1] = 0;
    fm.back_count = 0;
    fm.forward_count = 0;
    fm.count = 0;
    fm.selected = -1;
    fm.hovered = -1;
    fm.last_click_idx = -1;
    fm.last_click_ticks = 0;
    fm.menu_visible = 0;
    fm.menu_x = 0;
    fm.menu_y = 0;
    fm.menu_target = -1;
    fm.dialog = FM_DIALOG_NONE;
    fm.dialog_target = -1;
    fm.input_len = 0;
    fm.input_buf[0] = 0;
    fm.rename_active = 0;
    fm_refresh();
}

static void fm_navigate(const char* new_path) {
    if (fm.back_count < FM_STACK_DEPTH) {
        my_strcpy(fm.back_stack[fm.back_count], fm.cwd, MAX_PATH);
        fm.back_count++;
    }
    my_strcpy(fm.cwd, new_path, MAX_PATH);
    fm.forward_count = 0;
    fm_refresh();
}

static void fm_back(void) {
    if (fm.back_count == 0) return;
    if (fm.forward_count < FM_STACK_DEPTH) {
        my_strcpy(fm.forward_stack[fm.forward_count], fm.cwd, MAX_PATH);
        fm.forward_count++;
    }
    fm.back_count--;
    my_strcpy(fm.cwd, fm.back_stack[fm.back_count], MAX_PATH);
    fm_refresh();
}

static void fm_forward(void) {
    if (fm.forward_count == 0) return;
    if (fm.back_count < FM_STACK_DEPTH) {
        my_strcpy(fm.back_stack[fm.back_count], fm.cwd, MAX_PATH);
        fm.back_count++;
    }
    fm.forward_count--;
    my_strcpy(fm.cwd, fm.forward_stack[fm.forward_count], MAX_PATH);
    fm_refresh();
}

static void fm_up(void) {
    char parent[MAX_PATH];
    path_parent(fm.cwd, parent);
    if (my_streq(parent, fm.cwd)) return;
    fm_navigate(parent);
}

static void format_size(uint32_t bytes, char* out) {
    if (bytes < 1024) {
        int_to_str((int)bytes, out);
        int n = my_strlen(out);
        if (n + 1 < 16) { out[n] = 'B'; out[n + 1] = 0; }
    } else if (bytes < 1024 * 1024) {
        int_to_str((int)(bytes / 1024), out);
        int n = my_strlen(out);
        if (n + 1 < 16) { out[n] = 'K'; out[n + 1] = 0; }
    } else {
        int_to_str((int)(bytes / (1024 * 1024)), out);
        int n = my_strlen(out);
        if (n + 1 < 16) { out[n] = 'M'; out[n + 1] = 0; }
    }
}

static void draw_file_icon(int x, int y, const char* name, int isdir) {
    if (isdir) {
        draw_icon_folder_small(x, y);
        return;
    }
    uint32_t color = 0x00888888;
    char letter = '?';
    if (endswith_ci(name, ".txt")) { color = 0x005555BB; letter = 'T'; }
    else if (endswith_ci(name, ".mp3") || endswith_ci(name, ".wav")) { color = 0x00BB4444; letter = 'M'; }
    else if (endswith_ci(name, ".mp4")) { color = 0x008844CC; letter = 'V'; }
    else if (endswith_ci(name, ".bmp") || endswith_ci(name, ".jpg") || endswith_ci(name, ".jpeg")) {
        color = 0x0044AA44; letter = 'I';
    } else { color = 0x00888888; letter = 'F'; }
    fb_fillrect(x, y, 16, 16, color);
    fb_draw_rect_outline(x, y, 16, 16, 0x00444444);
    if (letter) draw_char(x + 5, y + 4, letter, COLOR_TEXT_WHITE);
}

static void fm_open_item(int idx) {
    if (idx < 0 || idx >= fm.count) return;
    char full[MAX_PATH];
    path_join(fm.cwd, fm.names[idx], full);
    if (fm.isdir[idx]) {
        fm_navigate(full);
        return;
    }
    if (endswith_ci(fm.names[idx], ".mp3") || endswith_ci(fm.names[idx], ".wav") ||
        endswith_ci(fm.names[idx], ".mp4")) {
        if (player_open(full) == 0) player_open_window();
        return;
    }
    if (endswith_ci(fm.names[idx], ".txt")) {
        window_t* gwin = window_find_by_type(WINDOW_TYPE_GSHELL);
        if (gwin && !gwin->visible) desktop_icon_open(WINDOW_TYPE_GSHELL);
        char cmdline[MAX_PATH + 8];
        int i = 0;
        const char* prefix = "cat ";
        while (prefix[i] && i < MAX_PATH + 7) { cmdline[i] = prefix[i]; i++; }
        int j = 0;
        while (full[j] && i < MAX_PATH + 7) cmdline[i++] = full[j++];
        cmdline[i] = 0;
        cmd_parse(cmdline);
        return;
    }
    if (endswith_ci(fm.names[idx], ".bmp") || endswith_ci(fm.names[idx], ".jpg") ||
        endswith_ci(fm.names[idx], ".jpeg")) {
        window_t* gwin = window_find_by_type(WINDOW_TYPE_GSHELL);
        if (gwin && !gwin->visible) desktop_icon_open(WINDOW_TYPE_GSHELL);
        gshell_puts("[Image Preview] ");
        gshell_puts(full);
        gshell_puts("\n");
        return;
    }
}

static const char* fm_menu_labels[] = {
    "打开", "复制", "粘贴", "删除", "重命名", "新建文件夹"
};
#define FM_MENU_COUNT 6
static int fm_menu_sep_before[] = { 0, 0, 0, 1, 0, 1 };

static void fm_menu_show(int mx, int my, int target) {
    fm.menu_visible = 1;
    fm.menu_x = mx;
    fm.menu_y = my;
    fm.menu_target = target;
}

static void fm_menu_hide(void) {
    fm.menu_visible = 0;
    fm.menu_target = -1;
}

static void fm_dialog_show(fm_dialog_t dlg, int target) {
    fm.dialog = dlg;
    fm.dialog_target = target;
    fm.input_len = 0;
    fm.input_buf[0] = 0;
    if (dlg == FM_DIALOG_RENAME && target >= 0 && target < fm.count) {
        my_strcpy(fm.input_buf, fm.names[target], FM_NAME_LEN);
        fm.input_len = my_strlen(fm.input_buf);
        fm.rename_active = 1;
    } else if (dlg == FM_DIALOG_NEW_FOLDER) {
        my_strcpy(fm.input_buf, "新建文件夹", FM_NAME_LEN);
        fm.input_len = my_strlen(fm.input_buf);
        fm.rename_active = 1;
    }
}

static void fm_dialog_close(void) {
    fm.dialog = FM_DIALOG_NONE;
    fm.dialog_target = -1;
    fm.rename_active = 0;
    fm.input_len = 0;
    fm.input_buf[0] = 0;
}

static void fm_do_delete(int idx) {
    if (idx < 0 || idx >= fm.count) return;
    char full[MAX_PATH];
    path_join(fm.cwd, fm.names[idx], full);
    fat32_remove(full);
    fm_refresh();
}

static void fm_do_rename(int idx) {
    if (idx < 0 || idx >= fm.count) return;
    if (fm.input_len == 0) return;
    char src[MAX_PATH];
    char dst[MAX_PATH];
    path_join(fm.cwd, fm.names[idx], src);
    path_join(fm.cwd, fm.input_buf, dst);
    int isdir = fm.isdir[idx];
    int srcfd = fat32_open(src, 0);
    if (srcfd >= 0) {
        uint8_t buf[2048];
        int n;
        static uint8_t fdata[65536];
        uint32_t fsize = fm.sizes[idx];
        uint32_t read = 0;
        while (read < fsize && (n = fat32_read(srcfd, buf, sizeof(buf))) > 0) {
            int to_copy = n;
            if (read + to_copy > sizeof(fdata)) to_copy = (int)(sizeof(fdata) - read);
            for (int i = 0; i < to_copy; i++) fdata[read + i] = buf[i];
            read += to_copy;
        }
        fat32_close(srcfd);
        fat32_remove(src);
        fat32_create(dst);
        int dfd = fat32_open(dst, 1);
        if (dfd >= 0) {
            (void)fat32_write(dfd, fdata, (int)read);
            fat32_close(dfd);
        }
    } else if (isdir) {
        fat32_mkdir(dst);
        fat32_remove(src);
    }
    fm_refresh();
}

static void fm_do_new_folder(void) {
    if (fm.input_len == 0) return;
    char full[MAX_PATH];
    path_join(fm.cwd, fm.input_buf, full);
    fat32_mkdir(full);
    fm_refresh();
}

static void fm_menu_activate(int item) {
    int target = fm.menu_target;
    fm_menu_hide();
    if (item == 0 && target >= 0) {
        fm_open_item(target);
    } else if (item == 3 && target >= 0) {
        fm_dialog_show(FM_DIALOG_DELETE, target);
    } else if (item == 4 && target >= 0) {
        fm_dialog_show(FM_DIALOG_RENAME, target);
    } else if (item == 5) {
        fm_dialog_show(FM_DIALOG_NEW_FOLDER, -1);
    }
}

static void fm_render_menu(void) {
    if (!fm.menu_visible) return;
    int w = 140;
    int h = FM_MENU_COUNT * 24 + 4;
    int x = fm.menu_x;
    int y = fm.menu_y;
    if (x + w > (int)fb_width) x = (int)fb_width - w;
    if (y + h > (int)fb_height) y = (int)fb_height - h;
    fb_fillrect(x, y, w, h, COLOR_MENU_BG);
    fb_draw_rect_outline(x, y, w, h, COLOR_MENU_BORDER);
    for (int i = 0; i < FM_MENU_COUNT; i++) {
        int item_y = y + 2 + i * 24;
        if (fm_menu_sep_before[i]) {
            fb_fillrect(x + 4, item_y, w - 8, 1, COLOR_MENU_BORDER);
        }
        int hover = (fm.hovered == i);
        uint32_t bg = hover ? COLOR_MENU_HOVER : COLOR_MENU_BG;
        fb_fillrect(x + 2, item_y + 1, w - 4, 22, bg);
        draw_string(x + 12, item_y + 4, fm_menu_labels[i], COLOR_MENU_TEXT);
    }
}

static void fm_render_dialog(window_t* win) {
    if (fm.dialog == FM_DIALOG_NONE) return;
    int cx = win->x + win->w / 2 - 140;
    int cy = win->y + win->h / 2 - 60;
    int cw = 280;
    int ch = 120;
    fb_fillrect(cx, cy, cw, ch, COLOR_WINDOW_BODY);
    fb_draw_rect_outline(cx, cy, cw, ch, 0x00888888);
    const char* title = "确认";
    const char* prompt = "确定吗？";
    if (fm.dialog == FM_DIALOG_DELETE) {
        title = "删除";
        prompt = "删除此项目？";
    } else if (fm.dialog == FM_DIALOG_RENAME) {
        title = "重命名";
        prompt = "输入新名称：";
    } else if (fm.dialog == FM_DIALOG_NEW_FOLDER) {
        title = "新建文件夹";
        prompt = "输入文件夹名称：";
    }
    fb_fillrect(cx, cy, cw, 22, 0x004466AA);
    draw_string_bold(cx + 8, cy + 3, title, COLOR_TEXT_WHITE);
    draw_string(cx + 12, cy + 32, prompt, COLOR_TEXT_BLACK);

    if (fm.dialog == FM_DIALOG_RENAME || fm.dialog == FM_DIALOG_NEW_FOLDER) {
        fb_fillrect(cx + 12, cy + 56, cw - 24, 24, COLOR_TEXT_WHITE);
        fb_draw_rect_outline(cx + 12, cy + 56, cw - 24, 24, 0x00888888);
        draw_string(cx + 16, cy + 60, fm.input_buf, COLOR_TEXT_BLACK);
        if ((timer_ticks / 25) % 2 == 0) {
            int cur_x = cx + 16 + fm.input_len * FONT_WIDTH;
            fb_fillrect(cur_x, cy + 60, 2, FONT_HEIGHT, COLOR_TEXT_BLACK);
        }
    }

    int btn_w = 80;
    int btn_h = 24;
    int yes_x = cx + 30;
    int no_x = cx + cw - btn_w - 30;
    int btn_y = cy + ch - btn_h - 12;
    if (fm.dialog == FM_DIALOG_RENAME || fm.dialog == FM_DIALOG_NEW_FOLDER) {
        btn_y = cy + ch - btn_h - 12;
    }
    fb_fillrect(yes_x, btn_y, btn_w, btn_h, 0x004466AA);
    draw_string_centered(yes_x, btn_y + 4, btn_w, "确定", COLOR_TEXT_WHITE);
    fb_fillrect(no_x, btn_y, btn_w, btn_h, 0x00888888);
    draw_string_centered(no_x, btn_y + 4, btn_w, "取消", COLOR_TEXT_WHITE);
}

void render_file_manager(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, 8, g_theme.bg);
    render_title_bar(win, g_theme.title_bar);

    int tb_y = y + 32;
    fb_fillrect(x, tb_y, w, FM_TOOLBAR_H, g_theme.title_bar_bottom);
    int btn_y = tb_y + (FM_TOOLBAR_H - FM_BTN_H) / 2;
    int bx = x + 8;
    fb_fillrect(bx, btn_y, FM_BTN_W, FM_BTN_H,
        fm.back_count ? g_theme.accent : 0x00AAAAAA);
    draw_string_centered(bx, btn_y + 3, FM_BTN_W, "<", COLOR_TEXT_WHITE);
    bx += FM_BTN_W + 4;
    fb_fillrect(bx, btn_y, FM_BTN_W, FM_BTN_H,
        fm.forward_count ? g_theme.accent : 0x00AAAAAA);
    draw_string_centered(bx, btn_y + 3, FM_BTN_W, ">", COLOR_TEXT_WHITE);
    bx += FM_BTN_W + 4;
    fb_fillrect(bx, btn_y, FM_BTN_W, FM_BTN_H, g_theme.accent);
    draw_string_centered(bx, btn_y + 3, FM_BTN_W, "^", COLOR_TEXT_WHITE);

    int addr_x = x + FM_ADDR_X;
    int addr_w = w - FM_ADDR_X - 8;
    if (addr_w < 80) addr_w = 80;
    fb_fillrect(addr_x, btn_y, addr_w, FM_BTN_H, COLOR_TEXT_WHITE);
    fb_draw_rect_outline(addr_x, btn_y, addr_w, FM_BTN_H, 0x00888888);
    draw_string(addr_x + 6, btn_y + 4, fm.cwd, g_theme.text);

    int hdr_y = tb_y + FM_TOOLBAR_H;
    fb_fillrect(x, hdr_y, w, FM_HEADER_H, g_theme.title_bar_bottom);
    draw_string_bold(x + FM_COL_NAME_X, hdr_y + 4, "名称", COLOR_TEXT_WHITE);
    draw_string_bold(x + FM_COL_SIZE_X, hdr_y + 4, "大小", COLOR_TEXT_WHITE);
    draw_string_bold(x + FM_COL_DATE_X, hdr_y + 4, "日期", COLOR_TEXT_WHITE);
    fb_fillrect(x + 8, hdr_y + FM_HEADER_H - 1, w - 16, 1, 0x00AAAAAA);

    int list_y = hdr_y + FM_HEADER_H;
    int row_h = FM_ROW_H;
    int max_rows = (h - (list_y - y) - FM_STATUS_H) / row_h;
    if (max_rows < 0) max_rows = 0;
    for (int i = 0; i < fm.count && i < max_rows; i++) {
        int ry = list_y + i * row_h;
        if (i == fm.selected) {
            fb_fillrect(x + 4, ry, w - 8, row_h, 0x00CCE8FF);
        } else if (i == fm.hovered && fm.menu_visible == 0 && fm.dialog == FM_DIALOG_NONE) {
            fb_fillrect(x + 4, ry, w - 8, row_h, 0x33AAAAAA);
        }
        draw_file_icon(x + 12, ry + 3, fm.names[i], fm.isdir[i]);
        draw_string(x + FM_COL_NAME_X, ry + 3, fm.names[i], g_theme.text);
        if (fm.isdir[i]) {
            draw_string(x + FM_COL_SIZE_X, ry + 3, "<DIR>", g_theme.text_secondary);
        } else {
            char sz[16];
            format_size(fm.sizes[i], sz);
            draw_string(x + FM_COL_SIZE_X, ry + 3, sz, g_theme.text);
        }
        draw_string(x + FM_COL_DATE_X, ry + 3, "2025/01", g_theme.text_secondary);
    }

    int st_y = y + h - FM_STATUS_H;
    fb_fillrect(x, st_y, w, FM_STATUS_H, g_theme.title_bar_bottom);
    char sb[64];
    int j = 0;
    const char* p = "Items: ";
    while (*p && j < 60) sb[j++] = *p++;
    int_to_str(fm.count, sb + j);
    while (sb[j] && j < 60) j++;
    if (fm.selected >= 0 && fm.selected < fm.count) {
        const char* sel = "  Selected: ";
        while (*sel && j < 60) sb[j++] = *sel++;
        char idxbuf[8];
        int_to_str(fm.selected, idxbuf);
        int k = 0;
        while (idxbuf[k] && j < 60) sb[j++] = idxbuf[k++];
    }
    sb[j] = 0;
    draw_string(x + 12, st_y + 3, sb, COLOR_TEXT_WHITE);

    fm_render_menu();
    fm_render_dialog(win);
}

void file_manager_handle_click(window_t* win, int mx, int my) {
    uint8_t buttons = mouse_buttons;
    int right = buttons & 0x02;

    if (fm.dialog != FM_DIALOG_NONE) {
        int cx = win->x + win->w / 2 - 140;
        int cy = win->y + win->h / 2 - 60;
        int cw = 280;
        int ch = 120;
        int btn_w = 80;
        int btn_h = 24;
        int btn_y = cy + ch - btn_h - 12;
        int yes_x = cx + 30;
        int no_x = cx + cw - btn_w - 30;
        if (mx >= yes_x && mx < yes_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
            if (fm.dialog == FM_DIALOG_DELETE) {
                fm_do_delete(fm.dialog_target);
            } else if (fm.dialog == FM_DIALOG_RENAME) {
                fm_do_rename(fm.dialog_target);
            } else if (fm.dialog == FM_DIALOG_NEW_FOLDER) {
                fm_do_new_folder();
            }
            fm_dialog_close();
            gui_dirty = 1;
            return;
        }
        if (mx >= no_x && mx < no_x + btn_w && my >= btn_y && my < btn_y + btn_h) {
            fm_dialog_close();
            gui_dirty = 1;
            return;
        }
        return;
    }

    if (fm.menu_visible) {
        int w = 140;
        int h = FM_MENU_COUNT * 24 + 4;
        int xx = fm.menu_x;
        int yy = fm.menu_y;
        if (xx + w > (int)fb_width) xx = (int)fb_width - w;
        if (yy + h > (int)fb_height) yy = (int)fb_height - h;
        if (mx >= xx && mx < xx + w && my >= yy && my < yy + h) {
            int item = (my - yy - 2) / 24;
            if (item >= 0 && item < FM_MENU_COUNT) fm_menu_activate(item);
            gui_dirty = 1;
            return;
        }
        fm_menu_hide();
        gui_dirty = 1;
        if (!right) return;
    }

    int tb_y = win->y + 32;
    int btn_y = tb_y + (FM_TOOLBAR_H - FM_BTN_H) / 2;
    int bx = win->x + 8;
    if (!right && my >= btn_y && my < btn_y + FM_BTN_H) {
        if (mx >= bx && mx < bx + FM_BTN_W) { fm_back(); gui_dirty = 1; return; }
        bx += FM_BTN_W + 4;
        if (mx >= bx && mx < bx + FM_BTN_W) { fm_forward(); gui_dirty = 1; return; }
        bx += FM_BTN_W + 4;
        if (mx >= bx && mx < bx + FM_BTN_W) { fm_up(); gui_dirty = 1; return; }
    }

    int hdr_y = tb_y + FM_TOOLBAR_H;
    int list_y = hdr_y + FM_HEADER_H;
    if (my < list_y) return;
    int row = (my - list_y) / FM_ROW_H;
    if (row < 0 || row >= fm.count) {
        if (right) {
            fm_menu_show(mx, my, -1);
            gui_dirty = 1;
        }
        fm.selected = -1;
        return;
    }

    if (right) {
        fm.selected = row;
        fm_menu_show(mx, my, row);
        gui_dirty = 1;
        return;
    }

    fm.selected = row;
    uint32_t now = timer_ticks;
    int double_click = (fm.last_click_idx == row &&
        (int)(now - fm.last_click_ticks) < 25);
    fm.last_click_idx = row;
    fm.last_click_ticks = now;
    if (double_click) {
        fm_open_item(row);
        fm.last_click_idx = -1;
    }
    gui_dirty = 1;
}

void file_manager_handle_key(char c, uint8_t sc) {
    if (!fm.rename_active) return;
    if (sc == 0x0E) {
        if (fm.input_len > 0) {
            fm.input_len--;
            fm.input_buf[fm.input_len] = 0;
            gui_dirty = 1;
        }
    } else if (sc == 0x1C) {
        if (fm.dialog == FM_DIALOG_RENAME) fm_do_rename(fm.dialog_target);
        else if (fm.dialog == FM_DIALOG_NEW_FOLDER) fm_do_new_folder();
        fm_dialog_close();
        gui_dirty = 1;
    } else if (sc == 0x01) {
        fm_dialog_close();
        gui_dirty = 1;
    } else if (c && c >= ' ' && c <= '~' && fm.input_len < FM_NAME_LEN - 1) {
        fm.input_buf[fm.input_len++] = c;
        fm.input_buf[fm.input_len] = 0;
        gui_dirty = 1;
    }
}

void file_manager_update_hover(int mx, int my) {
    if (fm.dialog != FM_DIALOG_NONE) return;
    fm.hovered = -1;
    if (fm.menu_visible) {
        int w = 140;
        int h = FM_MENU_COUNT * 24 + 4;
        int xx = fm.menu_x;
        int yy = fm.menu_y;
        if (xx + w > (int)fb_width) xx = (int)fb_width - w;
        if (yy + h > (int)fb_height) yy = (int)fb_height - h;
        if (mx >= xx && mx < xx + w && my >= yy && my < yy + h) {
            fm.hovered = (my - yy - 2) / 24;
        }
    }
}
