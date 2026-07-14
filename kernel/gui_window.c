#include "gui_internal.h"
#include "memory.h"
#include "idt.h"
#include "easing.h"
#include <stdint.h>
#include <stddef.h>

window_t windows[MAX_WINDOWS];
int window_count = 0;
int next_z_order = 1;

void window_bring_to_front_internal(window_t* win) {
    if (!win) return;
    int old_z = win->z_order;
    win->z_order = next_z_order++;
    for (int i = 0; i < window_count; i++) {
        if (&windows[i] != win && windows[i].z_order > old_z) {
            windows[i].z_order--;
        }
    }
}

void window_bring_to_front(window_t* win) {
    window_bring_to_front_internal(win);
}

window_t* window_get_at(int mx, int my) {
    window_t* top_window = NULL;
    int highest_z = -1;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].visible || windows[i].minimized) continue;
        if (mx >= windows[i].x && mx < windows[i].x + windows[i].w &&
            my >= windows[i].y && my < windows[i].y + windows[i].h) {
            if (windows[i].z_order > highest_z) {
                highest_z = windows[i].z_order;
                top_window = &windows[i];
            }
        }
    }
    return top_window;
}

window_t* window_top_visible(void) {
    window_t* top = NULL;
    int highest_z = -1;
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].visible || windows[i].minimized) continue;
        if (windows[i].z_order > highest_z) {
            highest_z = windows[i].z_order;
            top = &windows[i];
        }
    }
    return top;
}

window_t* window_find_by_type(int type) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i].type == type) return &windows[i];
    }
    return 0;
}

void init_window_system(void) {
    window_count = 0;
    next_z_order = 1;

    windows[window_count].type = WINDOW_TYPE_DEMO;
    windows[window_count].x = 200; windows[window_count].y = 150;
    windows[window_count].w = 400; windows[window_count].h = 300;
    windows[window_count].normal_x = 200; windows[window_count].normal_y = 150;
    windows[window_count].normal_w = 400; windows[window_count].normal_h = 300;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "窗口";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_GSHELL;
    windows[window_count].x = 50; windows[window_count].y = 50;
    windows[window_count].w = 700; windows[window_count].h = 500;
    windows[window_count].normal_x = 50; windows[window_count].normal_y = 50;
    windows[window_count].normal_w = 700; windows[window_count].normal_h = 500;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "终端";
    windows[window_count].data = &gshell;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_ABOUT;
    windows[window_count].x = 250; windows[window_count].y = 180;
    windows[window_count].w = 420; windows[window_count].h = 260;
    windows[window_count].normal_x = 250; windows[window_count].normal_y = 180;
    windows[window_count].normal_w = 420; windows[window_count].normal_h = 260;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "关于 NEO-OS";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_CALCULATOR;
    windows[window_count].x = 300; windows[window_count].y = 200;
    windows[window_count].w = 260; windows[window_count].h = 340;
    windows[window_count].normal_x = 300; windows[window_count].normal_y = 200;
    windows[window_count].normal_w = 260; windows[window_count].normal_h = 340;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "计算器";
    windows[window_count].data = &calc_state;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_BROWSER;
    windows[window_count].x = 80; windows[window_count].y = 40;
    windows[window_count].w = 800; windows[window_count].h = 600;
    windows[window_count].normal_x = 80; windows[window_count].normal_y = 40;
    windows[window_count].normal_w = 800; windows[window_count].normal_h = 600;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "浏览器";
    windows[window_count].data = &browser;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_TASKMGR;
    windows[window_count].x = 280; windows[window_count].y = 120;
    windows[window_count].w = 540; windows[window_count].h = 380;
    windows[window_count].normal_x = 280; windows[window_count].normal_y = 120;
    windows[window_count].normal_w = 540; windows[window_count].normal_h = 380;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "任务管理器";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_APP_STORE;
    windows[window_count].x = 120; windows[window_count].y = 70;
    windows[window_count].w = 820; windows[window_count].h = 480;
    windows[window_count].normal_x = 120; windows[window_count].normal_y = 70;
    windows[window_count].normal_w = 820; windows[window_count].normal_h = 480;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "应用商店";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_PLAYER;
    windows[window_count].x = 200; windows[window_count].y = 100;
    windows[window_count].w = 560; windows[window_count].h = 420;
    windows[window_count].normal_x = 200; windows[window_count].normal_y = 100;
    windows[window_count].normal_w = 560; windows[window_count].normal_h = 420;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "媒体播放器";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_FILE_MANAGER;
    windows[window_count].x = 120; windows[window_count].y = 80;
    windows[window_count].w = 640; windows[window_count].h = 440;
    windows[window_count].normal_x = 120; windows[window_count].normal_y = 80;
    windows[window_count].normal_w = 640; windows[window_count].normal_h = 440;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "文件管理器";
    windows[window_count].data = 0;
    window_count++;

    windows[window_count].type = WINDOW_TYPE_SETTINGS;
    windows[window_count].x = 180; windows[window_count].y = 100;
    windows[window_count].w = 560; windows[window_count].h = 400;
    windows[window_count].normal_x = 180; windows[window_count].normal_y = 100;
    windows[window_count].normal_w = 560; windows[window_count].normal_h = 400;
    windows[window_count].visible = 0;
    windows[window_count].minimized = 0;
    windows[window_count].dragging = 0;
    windows[window_count].z_order = next_z_order++;
    windows[window_count].title = "设置";
    windows[window_count].data = 0;
    window_count++;
}

void clamp_window(window_t* win) {
    int sw = (int)vbe_get_width();
    int sh = (int)vbe_get_height();
    if (!win) return;
    if (win->x < 0) win->x = 0;
    if (win->y < 0) win->y = 0;
    if (win->x + win->w > sw) win->x = sw - win->w;
    if (win->y + win->h > sh) win->y = sh - win->h;
    if (win->w > sw) win->x = 0;
    if (win->h > sh) win->y = 0;
}

static int lerp(int s, int e, int t) {
    return s + (e - s) * t / 256;
}

void window_start_animation(window_t* win, uint32_t duration,
                            int sx, int sy, int sw, int sh,
                            int ex, int ey, int ew, int eh,
                            int hide_on_finish) {
    if (!win) return;
    win->animating = 1;
    win->anim_start = timer_ticks;
    win->anim_duration = duration ? duration : 1;
    win->anim_sx = sx; win->anim_sy = sy; win->anim_sw = sw; win->anim_sh = sh;
    win->anim_ex = ex; win->anim_ey = ey; win->anim_ew = ew; win->anim_eh = eh;
    win->anim_hide_on_finish = hide_on_finish;
    win->x = sx; win->y = sy; win->w = sw; win->h = sh;
    win->visible = 1;
    win->minimized = 0;
}

static void window_finish_animation(window_t* win) {
    win->animating = 0;
    win->x = win->anim_ex;
    win->y = win->anim_ey;
    win->w = win->anim_ew;
    win->h = win->anim_eh;
    if (win->anim_hide_on_finish) {
        win->visible = 0;
        win->minimized = 0;
    }
}

void window_update_animations(void) {
    for (int i = 0; i < window_count; i++) {
        window_t* win = &windows[i];
        if (!win->animating) continue;
        uint32_t elapsed = timer_ticks - win->anim_start;
        if (elapsed >= win->anim_duration) {
            window_finish_animation(win);
            continue;
        }
        int t = (int)(elapsed * 1000 / win->anim_duration);
        if (t < 0) t = 0;
        if (t > 1000) t = 1000;
        int eased = ease_out_cubic(t);
        int progress = eased * 256 / 1000;
        win->x = lerp(win->anim_sx, win->anim_ex, progress);
        win->y = lerp(win->anim_sy, win->anim_ey, progress);
        win->w = lerp(win->anim_sw, win->anim_ew, progress);
        win->h = lerp(win->anim_sh, win->anim_eh, progress);
    }
}

static void render_window_with_clip(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    fb_reset_clip();
    fb_fillrect_alpha(win->x + 2, win->y + 2, win->w, win->h, 0x15000000);
    fb_fillrect_alpha(win->x + 4, win->y + 4, win->w, win->h, 0x20000000);
    fb_fillrect_alpha(win->x + 6, win->y + 6, win->w, win->h, 0x2C000000);
    fb_fillrect_alpha(win->x + 8, win->y + 8, win->w, win->h, 0x38000000);
    fb_set_clip(win->x, win->y, win->w, win->h);
    if (win->type == WINDOW_TYPE_GSHELL) render_gshell(win);
    else if (win->type == WINDOW_TYPE_DEMO) render_demo_window(win);
    else if (win->type == WINDOW_TYPE_ABOUT) render_about_window(win);
    else if (win->type == WINDOW_TYPE_CALCULATOR) render_calculator(win);
    else if (win->type == WINDOW_TYPE_BROWSER) render_browser(win);
    else if (win->type == WINDOW_TYPE_TASKMGR) render_taskmgr(win);
    else if (win->type == WINDOW_TYPE_DOWNLOADS) render_downloads(win);
    else if (win->type == WINDOW_TYPE_PLAYER) render_player(win);
    else if (win->type == WINDOW_TYPE_FILE_MANAGER) render_file_manager(win);
    else if (win->type == WINDOW_TYPE_SETTINGS) render_settings(win);
    else if (win->type == WINDOW_TYPE_APP_STORE) app_store_render(win);
    fb_reset_clip();
}

static int is_covered_by_above(int idx) {
    window_t* win = &windows[idx];
    int wx1 = win->x, wy1 = win->y;
    int wx2 = win->x + win->w, wy2 = win->y + win->h;
    for (int row = wy1; row < wy2; row++) {
        int xs[MAX_WINDOWS], xe[MAX_WINDOWS], cnt = 0;
        for (int k = idx + 1; k < window_count; k++) {
            if (!windows[k].visible || windows[k].minimized) continue;
            if (row < windows[k].y || row >= windows[k].y + windows[k].h) continue;
            int s = windows[k].x > wx1 ? windows[k].x : wx1;
            int e = windows[k].x + windows[k].w < wx2 ? windows[k].x + windows[k].w : wx2;
            if (s < e && cnt < MAX_WINDOWS) {
                xs[cnt] = s; xe[cnt] = e; cnt++;
            }
        }
        for (int a = 1; a < cnt; a++) {
            int vs = xs[a], ve = xe[a], b = a - 1;
            while (b >= 0 && xs[b] > vs) { xs[b+1] = xs[b]; xe[b+1] = xe[b]; b--; }
            xs[b+1] = vs; xe[b+1] = ve;
        }
        int covered = wx1;
        for (int a = 0; a < cnt; a++) {
            if (xs[a] > covered) break;
            if (xe[a] > covered) covered = xe[a];
        }
        if (covered < wx2) return 0;
    }
    return 1;
}

void window_render_all(void) {
    for (int i = 0; i < window_count - 1; i++) {
        for (int j = i + 1; j < window_count; j++) {
            if (windows[j].z_order < windows[i].z_order) {
                window_t tmp = windows[i];
                windows[i] = windows[j];
                windows[j] = tmp;
            }
        }
    }
    for (int i = 0; i < window_count; i++) {
        if (!windows[i].visible || windows[i].minimized) continue;
        if (i < window_count - 1 && is_covered_by_above(i)) continue;
        render_window_with_clip(&windows[i]);
    }
}
