#include "gui_internal.h"
#include "gfx_settings.h"
#include "demo_3d.h"
#include "wallpaper.h"
#include "mouse.h"
#include "memory.h"
#include "idt.h"
#include "easing.h"
#include "systray.h"
#include "notify.h"
#include "serial.h"
#include <stdint.h>

volatile int gui_active = 0;
volatile int gui_start_requested = 0;
static volatile int gui_exit_requested = 0;
volatile int gui_state = GUI_STATE_LOCKSCREEN;

volatile int gui_key_ready = 0;
volatile char gui_key_char = 0;
volatile uint8_t gui_key_scancode = 0;

volatile int gui_dirty = 1;
static volatile int drag_render = 0;

void render_desktop(void) {
    int mx = mouse_x;
    int my = mouse_y;

    serial_write("DESKTOP: wallpaper_render\n");
    wallpaper_render();
    serial_write("DESKTOP: render_desktop_icons\n");
    render_desktop_icons(mx, my);
    serial_write("DESKTOP: window_render_all\n");
    window_render_all();
    serial_write("DESKTOP: render_start_menu_new\n");
    render_start_menu_new();
    serial_write("DESKTOP: render_taskbar\n");
    render_taskbar(mx, my);
    serial_write("DESKTOP: render_power_menu\n");
    render_power_menu();
    serial_write("DESKTOP: render_context_menu\n");
    render_context_menu(&context_menu);
    serial_write("DESKTOP: render_context_menu taskbar\n");
    render_context_menu(&taskbar_menu);
    serial_write("DESKTOP: render_thumbnail\n");
    render_thumbnail();
    serial_write("DESKTOP: render_clock_bubble\n");
    render_clock_bubble();
    serial_write("DESKTOP: gfx_settings\n");
    if (gfx_settings_is_open()) gfx_settings_render();
    serial_write("DESKTOP: systray_render\n");
    systray_render(0, (int)fb_height - TASKBAR_HEIGHT, (int)fb_width, TASKBAR_HEIGHT);
    serial_write("DESKTOP: notify_render\n");
    notify_render();
    serial_write("DESKTOP: done\n");
}

void render_gui(void) {
    if (gui_dirty) {
        dirty_clear();
        mark_dirty(0, 0, (int)fb_width, (int)fb_height);
        gui_dirty = 0;
    }
    if (gui_state == GUI_STATE_LOCKSCREEN) {
        render_lockscreen();
    } else if (gui_state == GUI_STATE_LOGIN) {
        render_login();
    } else {
        static int desktop_frame = 0;
        if (desktop_frame < 5) {
            serial_write("GUI: render_desktop called\n");
            desktop_frame++;
        }
        render_desktop();
    }
    fb_blit_dirty();
    dirty_clear();
}

void update_desktop(int mx, int my, uint8_t buttons) {
    static uint8_t prev_buttons = 0;
    int left_down = (buttons & 0x01) && !(prev_buttons & 0x01);
    int right_down = (buttons & 0x02) && !(prev_buttons & 0x02);
    int left_held = buttons & 0x01;
    static window_t* drag_win = 0;

    if (drag_win) {
        if (!left_held) {
            drag_win->normal_x = drag_win->x;
            drag_win->normal_y = drag_win->y;
            drag_win->dragging = 0;
            drag_win = 0;
            gui_dirty = 1;
        } else {
            int old_dx = drag_win->x;
            int old_dy = drag_win->y;
            drag_win->x = mx - drag_win->drag_off_x;
            drag_win->y = my - drag_win->drag_off_y;
            clamp_window(drag_win);
            if (old_dx != drag_win->x || old_dy != drag_win->y) {
                mark_dirty(old_dx, old_dy, drag_win->w, drag_win->h);
                mark_dirty(drag_win->x, drag_win->y, drag_win->w, drag_win->h);
                drag_render = 1;
            }
        }
    } else if (right_down) {
        if (systray_handle_click(mx, my)) {
            prev_buttons = buttons;
            return;
        }
        if (context_menu.visible) context_menu_hide();
        if (taskbar_menu.visible) taskbar_menu_hide();
        if (power_menu_is_visible()) power_menu_hide();

        int ty = (int)fb_height - TASKBAR_HEIGHT;
        if (my >= ty) {
            taskbar_menu_show(mx, my);
        } else {
            window_t* win = window_get_at(mx, my);
            if (!win) context_menu_show(mx, my);
            else if (win->type == WINDOW_TYPE_FILE_MANAGER) {
                file_manager_handle_click(win, mx, my);
            }
        }
        prev_buttons = buttons;
        return;
    } else if (left_down) {
        if (systray_handle_click(mx, my)) {
            prev_buttons = buttons;
            return;
        }
        if (gfx_settings_is_open()) {
            gfx_settings_handle_click(mx, my);
            prev_buttons = buttons;
            return;
        }
        if (context_menu.visible) {
            context_menu_handle_click(mx, my);
            prev_buttons = buttons;
            return;
        }
        if (power_menu_is_visible()) {
            power_menu_handle_click(mx, my);
            prev_buttons = buttons;
            return;
        }
        if (taskbar_menu.visible) {
            taskbar_menu_handle_click(mx, my);
            prev_buttons = buttons;
            return;
        }
        if (start_menu_is_visible()) {
            start_menu_handle_click(mx, my);
            prev_buttons = buttons;
            return;
        }

        int ty = (int)fb_height - TASKBAR_HEIGHT;
        int start_btn_x = 8;
        int start_btn_y = ty + (TASKBAR_HEIGHT - START_BTN_SIZE) / 2;
        if (mx >= start_btn_x && mx < start_btn_x + START_BTN_SIZE &&
            my >= start_btn_y && my < start_btn_y + START_BTN_SIZE) {
            if (power_menu_is_visible()) power_menu_hide();
            start_menu_toggle();
            prev_buttons = buttons;
            return;
        }

        if (my >= ty) {
            int btn_w = TASKBAR_BTN_W;
            int btn_h = TASKBAR_BTN_H;
            int list_start = taskbar_list_start();
            int idx = taskbar_button_at(mx, my);
            if (idx >= 0 && idx < window_count) {
                window_t* win = &windows[idx];
                if (!win->visible || win->minimized) {
                    int ex = win->normal_x, ey = win->normal_y, ew = win->normal_w, eh = win->normal_h;
                    int sx = list_start + idx * (btn_w + TASKBAR_BTN_GAP) + btn_w / 2 - 10;
                    int sy = ty + (TASKBAR_HEIGHT - btn_h) / 2 + btn_h / 2 - 10;
                    win->minimized = 0;
                    window_bring_to_front_internal(win);
                    window_start_animation(win, 12, sx, sy, 20, 20, ex, ey, ew, eh, 0);
                }
            }
            prev_buttons = buttons;
            return;
        }

        desktop_icon_handle_click(mx, my);

        window_t* win = window_get_at(mx, my);
        if (win) {
            int cx = win->x + win->w - 28;
            int cy = win->y;
            int restx = win->x + win->w - 56;
            int resty = win->y;
            int minx = win->x + win->w - 84;
            int miny = win->y;
            if (mx >= cx && mx < cx + 28 && my >= cy && my < cy + 32) {
                int idx = (int)(win - windows);
                int btn_w = TASKBAR_BTN_W, btn_h = TASKBAR_BTN_H;
                int list_start = taskbar_list_start();
                int sx = list_start + idx * (btn_w + TASKBAR_BTN_GAP) + btn_w / 2 - 10;
                int sy = ty + (TASKBAR_HEIGHT - btn_h) / 2 + btn_h / 2 - 10;
                window_start_animation(win, 10, win->x, win->y, win->w, win->h, sx, sy, 20, 20, 1);
                prev_buttons = buttons;
                return;
            }
            if (mx >= restx && mx < restx + 28 && my >= resty && my < resty + 32) {
                int sw = (int)fb_width;
                int sh = (int)fb_height - TASKBAR_HEIGHT;
                if (win->w == win->normal_w && win->h == win->normal_h) {
                    window_start_animation(win, 10, win->x, win->y, win->w, win->h, 0, 0, sw, sh, 0);
                } else {
                    window_start_animation(win, 10, win->x, win->y, win->w, win->h, win->normal_x, win->normal_y, win->normal_w, win->normal_h, 0);
                }
                prev_buttons = buttons;
                return;
            }
            if (mx >= minx && mx < minx + 28 && my >= miny && my < miny + 32) {
                int idx = (int)(win - windows);
                int btn_w = TASKBAR_BTN_W, btn_h = TASKBAR_BTN_H;
                int list_start = taskbar_list_start();
                int sx = list_start + idx * (btn_w + TASKBAR_BTN_GAP) + btn_w / 2 - 10;
                int sy = ty + (TASKBAR_HEIGHT - btn_h) / 2 + btn_h / 2 - 10;
                window_start_animation(win, 10, win->x, win->y, win->w, win->h, sx, sy, 20, 20, 1);
                win->minimized = 1;
                prev_buttons = buttons;
                return;
            }
            if (my >= win->y && my < win->y + 32) {
                window_bring_to_front_internal(win);
                win->dragging = 1;
                drag_win = win;
                win->drag_off_x = mx - win->x;
                win->drag_off_y = my - win->y;
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_CALCULATOR && my >= win->y + 32) {
                calculator_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_BROWSER && my >= win->y + 32) {
                browser_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_TASKMGR && my >= win->y + 32) {
                taskmgr_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_DOWNLOADS && my >= win->y + 32) {
                downloads_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_PLAYER && my >= win->y + 32) {
                player_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_FILE_MANAGER && my >= win->y + 32) {
                file_manager_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_SETTINGS && my >= win->y + 32) {
                settings_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
            if (win->type == WINDOW_TYPE_APP_STORE && my >= win->y + 32) {
                app_store_handle_click(win, mx, my);
                prev_buttons = buttons;
                return;
            }
        }
    }

    if (start_menu_is_visible()) {
        start_menu_hover_update(mx, my);
    }

    if (power_menu_is_visible()) {
        power_menu_update_hover(mx, my);
    }

    if (context_menu.visible) {
        int item_h = START_MENU_ITEM_HEIGHT;
        int idx = (my - context_menu.y) / item_h;
        if (idx >= 0 && idx < context_menu.item_count && idx != context_menu.selected_item) {
            context_menu.selected_item = idx;
            gui_dirty = 1;
        }
    }

    if (taskbar_menu.visible) {
        int item_h = START_MENU_ITEM_HEIGHT;
        int idx = (my - taskbar_menu.y) / item_h;
        if (idx >= 0 && idx < taskbar_menu.item_count && idx != taskbar_menu.selected_item) {
            taskbar_menu.selected_item = idx;
            gui_dirty = 1;
        }
    }

    systray_handle_hover(mx, my);
    prev_buttons = buttons;
}

void gui_handle_key(char c, uint8_t sc) {
    if (gui_state == GUI_STATE_LOCKSCREEN) {
        gui_state = GUI_STATE_LOGIN;
        return;
    }
    if (gui_state == GUI_STATE_LOGIN) {
        login_handle_key(c, sc);
        return;
    }

    if (sc == 0x01) {
        if (start_menu_is_visible()) start_menu_hide();
        else {
            window_t* top = window_top_visible();
            if (top) top->visible = 0;
        }
        return;
    }
    if (sc == 0x5B || sc == 0x5C) {
        start_menu_toggle();
        return;
    }

    window_t* top = window_top_visible();
    if (top && top->type == WINDOW_TYPE_BROWSER) {
        browser_handle_key(c, sc);
        return;
    }
    if (top && top->type == WINDOW_TYPE_FILE_MANAGER) {
        file_manager_handle_key(c, sc);
        return;
    }

    if (sc == 0x0E) gshell_input_backspace();
    else if (sc == 0x1C) gshell_input_submit();
    else if (c) gshell_input_char(c);
}

void gui_request_exit(void) {
    gui_exit_requested = 1;
}

void gui_run(void) {
    gui_exit_requested = 0;
    gui_active = 1;
    gui_state = GUI_STATE_LOCKSCREEN;

    if (!fb_init()) {
        gui_active = 0;
        return;
    }

    // 开机 logo 显示
    fb_show_boot_logo();
    for (volatile int d = 0; d < 30000000; d++);

    mouse_init();
    init_window_system();
    browser_init();
    download_init();
    player_init();
    file_manager_init();
    settings_init();
    start_menu_init();
    context_menu_init();
    taskbar_menu_init();
    init_desktop_icons();

    gshell_puts("GShell ready.\nGSHELL> ");

    // desktop_icon_open(WINDOW_TYPE_BROWSER);
    // browser_navigate("https://neocn.top");

    render_gui();
    fb_blit();
    gui_dirty = 0;

    int prev_mx = mouse_x;
    int prev_my = mouse_y;
    uint8_t prev_buttons = mouse_buttons;
    int prev_seconds = (int)(timer_ticks / 50);
    int cursor_x = mouse_x;
    int cursor_y = mouse_y;
    cursor_save_bg(cursor_x, cursor_y);
    cursor_draw_front(cursor_x, cursor_y);

    uint32_t last_proc_tick = 0;
    int input_pending = 0;

    while (!gui_exit_requested) {
        __asm__ volatile("hlt");
        int mx = mouse_x;
        int my = mouse_y;
        uint8_t buttons = mouse_buttons;
        int now_seconds = (int)(timer_ticks / 50);
        int key_handled = 0;

        if (gui_key_ready) {
            gui_key_ready = 0;
            gui_handle_key(gui_key_char, gui_key_scancode);
            key_handled = 1;
        }

        if (now_seconds != prev_seconds) {
            prev_seconds = now_seconds;
            gui_dirty = 1;
        }

        if (gui_state == GUI_STATE_LOCKSCREEN && (mx != prev_mx || my != prev_my || buttons != prev_buttons || key_handled)) {
            gui_state = GUI_STATE_LOGIN;
            gui_dirty = 1;
        }

        if (login_anim_active) gui_dirty = 1;

        window_update_animations();
        anim_update();
        notify_update();
        start_menu_update_animation();
        for (int i = 0; i < window_count; i++) {
            if (windows[i].animating) { gui_dirty = 1; break; }
        }
        if (start_menu_is_animating()) gui_dirty = 1;

        int pos_changed = (mx != prev_mx || my != prev_my);
        int btn_changed = (buttons != prev_buttons);
        int mouse_moved = pos_changed || btn_changed;

        if (mouse_moved) {
            input_pending = 1;
        }

        int do_process = 0;
        if (btn_changed) {
            do_process = 1;
            last_proc_tick = timer_ticks;
            input_pending = 0;
        } else if (input_pending) {
            if ((int)(timer_ticks - last_proc_tick) * 10 >= 8) {
                do_process = 1;
                last_proc_tick = timer_ticks;
                input_pending = 0;
            }
        }

        if (demo_3d_is_active()) gui_dirty = 1;

        player_poll();

        if (!gui_dirty && !key_handled && !mouse_moved && !input_pending) {
            continue;
        }

        drag_render = 0;
        if (do_process && (gui_state == GUI_STATE_DESKTOP || gui_state == GUI_STATE_LOGIN)) {
            update_desktop(mx, my, buttons);
        }

        int need_full = gui_dirty || key_handled || drag_render;

        if (demo_3d_is_active()) {
            gui_dirty = 0;
            cursor_restore_bg();
            demo_3d_render();
            cursor_save_bg(mx, my);
            cursor_draw_front(mx, my);
        } else if (need_full) {
            cursor_restore_bg();
            mark_dirty(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
            render_gui();
            cursor_save_bg(mx, my);
            cursor_draw_front(mx, my);
            mark_dirty(mx, my, CURSOR_W, CURSOR_H);
        } else if (mouse_moved) {
            gui_dirty = 0;
            cursor_restore_bg();
            mark_dirty(cursor_x, cursor_y, CURSOR_W, CURSOR_H);
            cursor_save_bg(mx, my);
            cursor_draw_front(mx, my);
            mark_dirty(mx, my, CURSOR_W, CURSOR_H);
        }

        cursor_x = mx;
        cursor_y = my;
        prev_mx = mx;
        prev_my = my;
        prev_buttons = buttons;
    }

    gui_active = 0;
    fb_free();
    vbe_disable();
}