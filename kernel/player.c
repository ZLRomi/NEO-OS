#include "player.h"
#include "gui_internal.h"
#include "audio.h"
#include "mp3.h"
#include "mp4.h"
#include "fat32.h"
#include "heap.h"
#include "idt.h"
#include "serial.h"
#include "mouse.h"
#include "theme.h"
#include <stdint.h>

player_manager_t player_mgr;

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

static int endswith_ci(const char* s, const char* suffix) {
    int sl = str_len(s);
    int su = str_len(suffix);
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

int player_is_supported(const char* filename) {
    if (!filename) return 0;
    return endswith_ci(filename, ".mp3") ||
           endswith_ci(filename, ".mp4") ||
           endswith_ci(filename, ".wav");
}

static void extract_basename(const char* path, char* out, int max) {
    int last = -1;
    int i = 0;
    while (path[i]) {
        if (path[i] == '/' || path[i] == '\\') last = i;
        i++;
    }
    int s = last + 1;
    int j = 0;
    while (path[s] && j < max - 1) out[j++] = path[s++];
    out[j] = '\0';
}

void player_init(void) {
    for (int i = 0; i < PLAYER_MAX_SESSIONS; i++) {
        player_mgr.sessions[i].used = 0;
        player_mgr.sessions[i].mode = PLAYER_MODE_NONE;
        player_mgr.sessions[i].state = PLAYER_STATE_IDLE;
        player_mgr.sessions[i].pcm_data = 0;
        player_mgr.sessions[i].audio_handle = -1;
    }
    player_mgr.active = -1;
    player_mgr.visible = 0;
    player_mgr.playlist_count = 0;
    player_mgr.playlist_expanded = 0;
    player_mgr.rotation = 0;
    player_mgr.dragging_seek = 0;
    player_mgr.dragging_volume = 0;
    for (int i = 0; i < PLAYER_PLAYLIST_SIZE; i++) {
        player_mgr.playlist[i][0] = '\0';
    }
}

static player_session_t* get_active_session(void) {
    if (player_mgr.active < 0 || player_mgr.active >= PLAYER_MAX_SESSIONS) return 0;
    if (!player_mgr.sessions[player_mgr.active].used) return 0;
    return &player_mgr.sessions[player_mgr.active];
}

static void session_reset(player_session_t* s) {
    s->mode = PLAYER_MODE_NONE;
    s->state = PLAYER_STATE_IDLE;
    s->filepath[0] = '\0';
    s->filename[0] = '\0';
    s->duration_ms = 0;
    s->position_ms = 0;
    s->sample_rate = 0;
    s->channels = 0;
    s->volume = 75;
    s->video_width = 0;
    s->video_height = 0;
    s->video_frame_idx = 0;
    s->video_frame_count = 0;
    s->has_pcm = 0;
    if (s->pcm_data) {
        kfree(s->pcm_data);
        s->pcm_data = 0;
    }
    if (s->audio_handle >= 0) {
        audio_stop(s->audio_handle);
        s->audio_handle = -1;
    }
}

int player_open(const char* path) {
    if (!path) return -1;
    player_session_t* s = 0;
    for (int i = 0; i < PLAYER_MAX_SESSIONS; i++) {
        if (!player_mgr.sessions[i].used) {
            s = &player_mgr.sessions[i];
            break;
        }
    }
    if (!s) {
        s = &player_mgr.sessions[0];
        session_reset(s);
    }
    session_reset(s);
    s->used = 1;
    s->volume = audio_get_volume();
    str_copy(s->filepath, path, PLAYER_URL_SIZE);
    extract_basename(path, s->filename, 64);

    int fd = fat32_open(path, 0);
    if (fd < 0) {
        serial_write("PLAYER: cannot open ");
        serial_write(path);
        serial_write("\n");
        s->state = PLAYER_STATE_ERROR;
        return -1;
    }

    uint32_t file_size = 0;
    {
        char tmp[1024];
        int n;
        while ((n = fat32_read(fd, tmp, sizeof(tmp))) > 0) {
            file_size += n;
        }
        fat32_close(fd);
    }

    if (file_size == 0 || file_size > 16 * 1024 * 1024) {
        serial_write("PLAYER: file too large or empty\n");
        s->state = PLAYER_STATE_ERROR;
        return -1;
    }

    void* file_data = kmalloc(file_size);
    if (!file_data) {
        serial_write("PLAYER: out of memory\n");
        s->state = PLAYER_STATE_ERROR;
        return -1;
    }

    fd = fat32_open(path, 0);
    if (fd < 0) {
        kfree(file_data);
        s->state = PLAYER_STATE_ERROR;
        return -1;
    }
    uint32_t read_total = 0;
    while (read_total < file_size) {
        int n = fat32_read(fd, (char*)file_data + read_total, file_size - read_total);
        if (n <= 0) break;
        read_total += n;
    }
    fat32_close(fd);

    if (endswith_ci(path, ".mp3")) {
        s->mode = PLAYER_MODE_AUDIO;
        s->pcm_data = (int16_t*)kmalloc(PLAYER_PCM_MAX * 2);
        if (!s->pcm_data) {
            kfree(file_data);
            s->state = PLAYER_STATE_ERROR;
            return -1;
        }
        int sr, ch;
        int total = mp3_decode_all(file_data, file_size, s->pcm_data,
                                    PLAYER_PCM_MAX, &sr, &ch);
        if (total <= 0) {
            serial_write("PLAYER: mp3 decode failed\n");
            kfree(file_data);
            s->state = PLAYER_STATE_ERROR;
            return -1;
        }
        s->pcm_samples = (uint32_t)total;
        s->sample_rate = sr;
        s->channels = ch;
        {
            uint32_t frames = (uint32_t)total / (uint32_t)(ch > 0 ? ch : 1);
            if (sr > 0) s->duration_ms = frames / (uint32_t)sr * 1000;
        }
        s->has_pcm = 1;
        serial_write("PLAYER: mp3 loaded sr=");
        char buf[16];
        int_to_str(sr, buf);
        serial_write(buf);
        serial_write(" samples=");
        int_to_str(total, buf);
        serial_write(buf);
        serial_write("\n");
    } else if (endswith_ci(path, ".mp4")) {
        s->mode = PLAYER_MODE_VIDEO;
        mp4_parser_t parser;
        mp4_parser_init(&parser, file_data, file_size);
        mp4_parse(&parser);
        mp4_track_t* vtrack = 0;
        if (mp4_get_video_track(&parser, &vtrack) && vtrack) {
            s->video_width = vtrack->width;
            s->video_height = vtrack->height;
            s->video_frame_count = vtrack->sample_count;
            s->duration_ms = (int)mp4_get_duration_ms(&parser);
        } else {
            s->video_width = 320;
            s->video_height = 240;
            s->video_frame_count = 60;
            s->duration_ms = 2000;
        }
        serial_write("PLAYER: mp4 loaded ");
        char buf[16];
        int_to_str(s->video_width, buf);
        serial_write(buf);
        serial_write("x");
        int_to_str(s->video_height, buf);
        serial_write(buf);
        serial_write("\n");
    } else {
        kfree(file_data);
        s->state = PLAYER_STATE_ERROR;
        return -1;
    }

    kfree(file_data);
    s->state = PLAYER_STATE_IDLE;

    for (int i = 0; i < PLAYER_MAX_SESSIONS; i++) {
        if (player_mgr.sessions[i].used && &player_mgr.sessions[i] != s) {
            session_reset(&player_mgr.sessions[i]);
        }
    }
    player_mgr.active = (int)(s - player_mgr.sessions);
    player_mgr.visible = 1;

    int dup = 0;
    for (int i = 0; i < player_mgr.playlist_count; i++) {
        int k = 0;
        int same = 1;
        while (s->filename[k] && player_mgr.playlist[i][k]) {
            if (s->filename[k] != player_mgr.playlist[i][k]) { same = 0; break; }
            k++;
        }
        if (same && s->filename[k] == '\0' && player_mgr.playlist[i][k] == '\0') {
            dup = 1;
            break;
        }
    }
    if (!dup && player_mgr.playlist_count < PLAYER_PLAYLIST_SIZE) {
        int k = 0;
        while (s->filename[k] && k < PLAYER_PLAYLIST_NAME - 1) {
            player_mgr.playlist[player_mgr.playlist_count][k] = s->filename[k];
            k++;
        }
        player_mgr.playlist[player_mgr.playlist_count][k] = '\0';
        player_mgr.playlist_count++;
    }

    gui_dirty = 1;
    return 0;
}

void player_play(void) {
    player_session_t* s = get_active_session();
    if (!s) return;
    if (s->state == PLAYER_STATE_PLAYING) return;
    if (s->state == PLAYER_STATE_PAUSED) {
        if (s->audio_handle >= 0) audio_resume(s->audio_handle);
        s->state = PLAYER_STATE_PLAYING;
        return;
    }
    if (s->mode == PLAYER_MODE_AUDIO && s->has_pcm && s->pcm_data) {
        s->audio_handle = audio_play(s->pcm_data, s->pcm_samples * 2,
                                      s->sample_rate, s->channels, 0, 0, 0);
    }
    s->state = PLAYER_STATE_PLAYING;
    gui_dirty = 1;
}

void player_pause(void) {
    player_session_t* s = get_active_session();
    if (!s) return;
    if (s->state == PLAYER_STATE_PLAYING) {
        if (s->audio_handle >= 0) audio_pause(s->audio_handle);
        s->state = PLAYER_STATE_PAUSED;
        gui_dirty = 1;
    } else if (s->state == PLAYER_STATE_PAUSED) {
        if (s->audio_handle >= 0) audio_resume(s->audio_handle);
        s->state = PLAYER_STATE_PLAYING;
        gui_dirty = 1;
    }
}

void player_stop(void) {
    player_session_t* s = get_active_session();
    if (!s) return;
    if (s->audio_handle >= 0) {
        audio_stop(s->audio_handle);
        s->audio_handle = -1;
    }
    s->state = PLAYER_STATE_STOPPED;
    s->position_ms = 0;
    s->video_frame_idx = 0;
    gui_dirty = 1;
}

void player_seek(int pos_ms) {
    player_session_t* s = get_active_session();
    if (!s) return;
    if (pos_ms < 0) pos_ms = 0;
    if (pos_ms > s->duration_ms) pos_ms = s->duration_ms;
    s->position_ms = pos_ms;
    if (s->mode == PLAYER_MODE_VIDEO) {
        if (s->video_frame_count > 0 && s->duration_ms > 0) {
            s->video_frame_idx = pos_ms * s->video_frame_count / s->duration_ms;
        }
    }
    gui_dirty = 1;
}

void player_set_volume(int vol) {
    player_session_t* s = get_active_session();
    if (!s) return;
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s->volume = vol;
    audio_set_master_volume(vol);
    gui_dirty = 1;
}

void player_poll(void) {
    player_session_t* s = get_active_session();
    if (!s || s->state != PLAYER_STATE_PLAYING) return;
    player_mgr.rotation = (player_mgr.rotation + 2) % 360;
    if (s->mode == PLAYER_MODE_AUDIO && s->audio_handle >= 0) {
        s->position_ms = (int)audio_get_position_ms(s->audio_handle);
        if (s->audio_handle >= 0 && !audio_is_present()) {
            s->state = PLAYER_STATE_STOPPED;
        }
    } else if (s->mode == PLAYER_MODE_VIDEO) {
        if (s->duration_ms > 0) {
            s->video_frame_idx++;
            if (s->video_frame_count > 0) {
                s->position_ms = s->video_frame_idx * s->duration_ms / s->video_frame_count;
            }
            if (s->video_frame_idx >= s->video_frame_count) {
                s->state = PLAYER_STATE_STOPPED;
                s->video_frame_idx = 0;
                s->position_ms = 0;
            }
        }
    }
    audio_poll();
    gui_dirty = 1;
}

static void format_mmss(int ms, char* out) {
    int total_s = ms / 1000;
    int mm = total_s / 60;
    int ss = total_s % 60;
    int i = 0;
    if (mm >= 10) out[i++] = '0' + mm / 10;
    out[i++] = '0' + mm % 10;
    out[i++] = ':';
    out[i++] = '0' + ss / 10;
    out[i++] = '0' + ss % 10;
    out[i] = '\0';
}

static const int sin_table_90[91] = {
    0, 17, 35, 52, 70, 87, 105, 122, 139, 156, 174, 191, 208, 225, 242, 259,
    276, 292, 309, 326, 342, 358, 374, 390, 406, 423, 438, 454, 469, 485, 500,
    515, 530, 544, 559, 574, 588, 602, 616, 629, 643, 656, 669, 682, 695, 707,
    719, 731, 743, 755, 766, 777, 788, 799, 809, 819, 829, 839, 848, 857, 866,
    875, 883, 891, 899, 906, 914, 921, 927, 934, 940, 946, 951, 956, 961, 966,
    970, 974, 978, 982, 985, 988, 991, 994, 996, 998, 999, 1000
};

static int sin_deg_x1000(int deg) {
    deg = deg % 360;
    if (deg < 0) deg += 360;
    if (deg <= 90) return sin_table_90[deg];
    if (deg <= 180) return sin_table_90[180 - deg];
    if (deg <= 270) return -sin_table_90[deg - 180];
    return -sin_table_90[360 - deg];
}

static int cos_deg_x1000(int deg) {
    return sin_deg_x1000(deg + 90);
}

static void draw_icon_prev(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 6, cy - 6, cx - 6, cy + 6, color);
    fb_draw_line(cx - 5, cy - 6, cx - 5, cy + 6, color);
    fb_draw_line(cx + 6, cy - 6, cx - 3, cy, color);
    fb_draw_line(cx - 3, cy, cx + 6, cy + 6, color);
    fb_draw_line(cx + 5, cy - 5, cx - 2, cy, color);
    fb_draw_line(cx - 2, cy, cx + 5, cy + 5, color);
}

static void draw_icon_play(int cx, int cy, uint32_t color) {
    for (int off = 0; off < 2; off++) {
        fb_draw_line(cx - 4 + off, cy - 6, cx + 6, cy, color);
        fb_draw_line(cx + 6, cy, cx - 4 + off, cy + 6, color);
        fb_draw_line(cx - 4 + off, cy - 6, cx - 4 + off, cy + 6, color);
    }
}

static void draw_icon_pause(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 3, cy - 6, cx - 3, cy + 6, color);
    fb_draw_line(cx - 2, cy - 6, cx - 2, cy + 6, color);
    fb_draw_line(cx + 2, cy - 6, cx + 2, cy + 6, color);
    fb_draw_line(cx + 3, cy - 6, cx + 3, cy + 6, color);
}

static void draw_icon_next(int cx, int cy, uint32_t color) {
    fb_draw_line(cx - 6, cy - 6, cx + 3, cy, color);
    fb_draw_line(cx + 3, cy, cx - 6, cy + 6, color);
    fb_draw_line(cx - 5, cy - 5, cx + 2, cy, color);
    fb_draw_line(cx + 2, cy, cx - 5, cy + 5, color);
    fb_draw_line(cx + 6, cy - 6, cx + 6, cy + 6, color);
    fb_draw_line(cx + 5, cy - 6, cx + 5, cy + 6, color);
}

static void draw_icon_stop(int cx, int cy, uint32_t color) {
    for (int off = 0; off < 7; off++) {
        fb_draw_line(cx - 3, cy - 3 + off, cx + 3, cy - 3 + off, color);
    }
}

void render_player(window_t* win) {
    if (!win || !win->visible || win->minimized) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    fb_draw_round_rect(x, y, w, h, g_theme.corner_radius, g_theme.bg);
    render_title_bar(win, g_theme.accent);

    player_session_t* s = get_active_session();

    if (!s) {
        draw_string_centered(x, y + 60, w, "未加载媒体", g_theme.text_secondary);
        draw_string_centered(x, y + 92, w, "使用 'play <路径>' 命令", g_theme.text_secondary);
        return;
    }

    int disc_top = y + 40;
    int disc_h = 150;
    fb_fillrect(x + 12, disc_top, w - 24, disc_h, 0x001A1A2E);

    int cx = x + w / 2;
    int cy = disc_top + disc_h / 2;
    int r = 60;
    int is_audio = (s->mode == PLAYER_MODE_AUDIO);

    if (s->mode == PLAYER_MODE_VIDEO) {
        int vw = w - 24;
        int vh = disc_h;
        fb_fillrect(x + 12, disc_top, vw, vh, 0x00000000);
        if (s->state == PLAYER_STATE_PLAYING || s->state == PLAYER_STATE_PAUSED) {
            int idx = s->video_frame_idx;
            uint32_t rv = (uint32_t)(idx * 37) & 0xFF;
            uint32_t gv = (uint32_t)(idx * 91) & 0xFF;
            uint32_t bv = (uint32_t)(idx * 173) & 0xFF;
            uint32_t vcolor = (rv << 16) | (gv << 8) | bv;
            int vcx = vw / 2;
            int vcy = vh / 2;
            int vrad = vh / 4;
            for (int py = 0; py < vh; py++) {
                for (int px = 0; px < vw; px++) {
                    int dx = px - vcx;
                    int dy = py - vcy;
                    uint32_t c = vcolor;
                    if (dx * dx + dy * dy < vrad * vrad) c = (c + 0x404040) & 0xFFFFFF;
                    fb_putpixel(x + 12 + px, disc_top + py, c);
                }
            }
        } else {
            draw_string_centered(x + 12, disc_top + vh / 2 - 8, vw, "视频", COLOR_TEXT_WHITE);
        }
    } else if (is_audio) {
        fb_fill_circle(cx, cy, r, 0x001A1A1A);
        fb_draw_circle(cx, cy, r, 0x00333333);
        for (int ring = 8; ring < r; ring += 6) {
            fb_draw_circle(cx, cy, ring, 0x00262626);
        }
        fb_fill_circle(cx, cy, r - 12, 0x00B22222);
        fb_draw_circle(cx, cy, r - 12, 0x008B0000);
        int rot = player_mgr.rotation;
        int sx = (cos_deg_x1000(rot) * (r - 4)) / 1000;
        int sy = (sin_deg_x1000(rot) * (r - 4)) / 1000;
        fb_draw_line(cx, cy, cx + sx, cy + sy, 0x00DDDDDD);
        int sx2 = (cos_deg_x1000(rot + 120) * (r - 4)) / 1000;
        int sy2 = (sin_deg_x1000(rot + 120) * (r - 4)) / 1000;
        fb_draw_line(cx, cy, cx + sx2, cy + sy2, 0x00999999);
        int sx3 = (cos_deg_x1000(rot + 240) * (r - 4)) / 1000;
        int sy3 = (sin_deg_x1000(rot + 240) * (r - 4)) / 1000;
        fb_draw_line(cx, cy, cx + sx3, cy + sy3, 0x00999999);
        fb_fill_circle(cx, cy, 6, 0x00FFFFFF);
        fb_fill_circle(cx, cy, 3, 0x001A1A1A);
    } else {
        draw_string_centered(x + 12, cy - 8, w - 24, "无媒体", COLOR_TEXT_GRAY);
    }

    int name_y = disc_top + disc_h + 8;
    draw_string_centered_bold(x, name_y, w, s->filename, g_theme.text);

    int time_y = name_y + 26;
    char pos_str[16], dur_str[16];
    format_mmss(s->position_ms, pos_str);
    format_mmss(s->duration_ms, dur_str);
    draw_string(x + 24, time_y, pos_str, g_theme.text_secondary);
    int dur_len = str_len(dur_str);
    draw_string(x + w - 24 - dur_len * FONT_WIDTH, time_y, dur_str, g_theme.text_secondary);

    int bar_x = x + 24;
    int bar_w = w - 48;
    int bar_y = time_y + 18;
    int bar_h = 10;
    fb_fillrect(bar_x, bar_y, bar_w, bar_h, g_theme.border);
    int fill_w = 0;
    if (s->duration_ms > 0) fill_w = bar_w * s->position_ms / s->duration_ms;
    int bars = 36;
    int bar_gap = 2;
    int bar_unit = (bar_w - (bars - 1) * bar_gap) / bars;
    if (bar_unit < 2) bar_unit = 2;
    for (int i = 0; i < bars; i++) {
        int bx = bar_x + i * (bar_unit + bar_gap);
        int bh = 6 + (i * 7 + (timer_ticks >> 1)) % 6;
        if (s->state != PLAYER_STATE_PLAYING) bh = 6;
        int by = bar_y + (bar_h - bh) / 2;
        uint32_t c = (bx - bar_x < fill_w) ? g_theme.accent : 0x00888888;
        fb_fillrect(bx, by, bar_unit, bh, c);
    }
    if (fill_w > 0 && fill_w < bar_w) {
        fb_draw_line(bar_x + fill_w, bar_y - 2, bar_x + fill_w, bar_y + bar_h + 2, g_theme.text);
    }
    if (player_mgr.dragging_seek && (mouse_buttons & 0x01)) {
        int rel = mouse_x - bar_x;
        if (rel < 0) rel = 0;
        if (rel > bar_w) rel = bar_w;
        if (s->duration_ms > 0) {
            int pos = rel * s->duration_ms / bar_w;
            player_seek(pos);
        }
    }
    if (!(mouse_buttons & 0x01)) {
        player_mgr.dragging_seek = 0;
        player_mgr.dragging_volume = 0;
    }

    int btn_w = 44, btn_h = 28, btn_gap = 10;
    int total_btn = btn_w * 4 + btn_gap * 3;
    int start_x = x + (w - total_btn) / 2;
    int btn_y = bar_y + bar_h + 14;
    int mx = mouse_x, my = mouse_y;

    int btn_centers[4][2];
    for (int i = 0; i < 4; i++) {
        int bx = start_x + i * (btn_w + btn_gap);
        btn_centers[i][0] = bx + btn_w / 2;
        btn_centers[i][1] = btn_y + btn_h / 2;
        int hover = (mx >= bx && mx < bx + btn_w && my >= btn_y && my < btn_y + btn_h);
        uint32_t bg = hover ? g_theme.accent_hover : g_theme.bg;
        if (i == 1 && s->state == PLAYER_STATE_PLAYING) bg = g_theme.accent;
        fb_draw_round_rect(bx, btn_y, btn_w, btn_h, 6, bg);
        fb_draw_rect_outline(bx, btn_y, btn_w, btn_h, g_theme.border);
    }
    draw_icon_prev(btn_centers[0][0], btn_centers[0][1], g_theme.text);
    if (s->state == PLAYER_STATE_PLAYING) {
        draw_icon_pause(btn_centers[1][0], btn_centers[1][1], COLOR_TEXT_WHITE);
    } else {
        draw_icon_play(btn_centers[1][0], btn_centers[1][1], COLOR_TEXT_WHITE);
    }
    draw_icon_next(btn_centers[2][0], btn_centers[2][1], g_theme.text);
    draw_icon_stop(btn_centers[3][0], btn_centers[3][1], g_theme.text);

    int vol_label_w = 36;
    int vol_x = x + 24;
    int vol_y = btn_y + btn_h + 12;
    int vol_bar_x = vol_x + vol_label_w;
    int vol_bar_w = w - 48 - vol_label_w - 100;
    draw_string(vol_x, vol_y + 4, "音量", g_theme.text_secondary);
    fb_fillrect(vol_bar_x, vol_y + 8, vol_bar_w, 8, g_theme.border);
    fb_draw_round_rect(vol_bar_x, vol_y + 8, vol_bar_w, 8, 4, g_theme.border);
    int vfill = vol_bar_w * s->volume / 100;
    if (vfill > 0) {
        fb_fillrect(vol_bar_x, vol_y + 8, vfill, 8, g_theme.accent);
    }
    int vknob = vol_bar_x + vfill;
    fb_fill_circle(vknob, vol_y + 12, 6, g_theme.text);
    if (player_mgr.dragging_volume && (mouse_buttons & 0x01)) {
        int rel = mouse_x - vol_bar_x;
        if (rel < 0) rel = 0;
        if (rel > vol_bar_w) rel = vol_bar_w;
        int vol = rel * 100 / vol_bar_w;
        player_set_volume(vol);
    }
    char vol_str[8];
    int_to_str(s->volume, vol_str);
    int vl = str_len(vol_str);
    vol_str[vl] = '%';
    vol_str[vl + 1] = '\0';
    draw_string(vol_bar_x + vol_bar_w + 8, vol_y + 4, vol_str, g_theme.text);

    int pl_toggle_x = x + w - 24 - FONT_WIDTH * 5;
    int pl_toggle_y = vol_y;
    int pl_hover = (mx >= pl_toggle_x && mx < pl_toggle_x + FONT_WIDTH * 5 &&
                   my >= pl_toggle_y && my < pl_toggle_y + 16);
    if (pl_hover) {
        fb_fillrect(pl_toggle_x - 2, pl_toggle_y - 2, FONT_WIDTH * 5 + 4, 20, g_theme.border);
    }
    draw_string(pl_toggle_x, pl_toggle_y + 2, player_mgr.playlist_expanded ? "列表 v" : "列表 >", g_theme.accent);

    if (player_mgr.playlist_expanded) {
        int pl_y = vol_y + 24;
        int pl_h = h - (pl_y - y) - 8;
        if (pl_h < 0) pl_h = 0;
        fb_fillrect(x + 12, pl_y, w - 24, pl_h, g_theme.bg);
        fb_draw_rect_outline(x + 12, pl_y, w - 24, pl_h, g_theme.border);
        int row_h = 20;
        for (int i = 0; i < player_mgr.playlist_count; i++) {
            int ry = pl_y + 4 + i * row_h;
            if (ry + row_h > pl_y + pl_h) break;
            int is_cur = 0;
            if (s->filename[0]) {
                int k = 0;
                int same = 1;
                while (s->filename[k] && player_mgr.playlist[i][k]) {
                    if (s->filename[k] != player_mgr.playlist[i][k]) { same = 0; break; }
                    k++;
                }
                if (same && s->filename[k] == '\0' && player_mgr.playlist[i][k] == '\0') is_cur = 1;
            }
            uint32_t c = is_cur ? g_theme.accent : g_theme.text;
            if (is_cur) {
                fb_fillrect(x + 14, ry - 1, w - 28, row_h - 2, 0x33888888);
            }
            char idx_str[8];
            int_to_str(i + 1, idx_str);
            draw_string(x + 18, ry + 2, idx_str, g_theme.text_secondary);
            draw_string(x + 18 + FONT_WIDTH * 3, ry + 2, player_mgr.playlist[i], c);
        }
        if (player_mgr.playlist_count == 0) {
            draw_string_centered(x + 12, pl_y + 8, w - 24, "播放列表为空", g_theme.text_secondary);
        }
    }
}

void player_handle_click(window_t* win, int mx, int my) {
    if (!win) return;
    int x = win->x, y = win->y, w = win->w, h = win->h;
    (void)h;

    player_session_t* s = get_active_session();
    if (!s) return;

    int disc_top = y + 40;
    int disc_h = 150;
    int name_y = disc_top + disc_h + 8;
    int time_y = name_y + 26;
    int bar_x = x + 24;
    int bar_w = w - 48;
    int bar_y = time_y + 18;
    int bar_h = 10;

    if (my >= bar_y - 4 && my < bar_y + bar_h + 4 && mx >= bar_x && mx < bar_x + bar_w) {
        player_mgr.dragging_seek = 1;
        player_mgr.dragging_volume = 0;
        int rel = mx - bar_x;
        if (rel < 0) rel = 0;
        if (rel > bar_w) rel = bar_w;
        if (s->duration_ms > 0) {
            int pos = rel * s->duration_ms / bar_w;
            player_seek(pos);
        }
        gui_dirty = 1;
        return;
    }

    int btn_w = 44, btn_h = 28, btn_gap = 10;
    int total_btn = btn_w * 4 + btn_gap * 3;
    int start_x = x + (w - total_btn) / 2;
    int btn_y = bar_y + bar_h + 14;

    if (my >= btn_y && my < btn_y + btn_h) {
        for (int i = 0; i < 4; i++) {
            int bx = start_x + i * (btn_w + btn_gap);
            if (mx >= bx && mx < bx + btn_w) {
                if (i == 0) {
                    int prev = player_mgr.active - 1;
                    while (prev >= 0 && !player_mgr.sessions[prev].used) prev--;
                    if (prev >= 0) {
                        player_mgr.active = prev;
                        gui_dirty = 1;
                    }
                } else if (i == 1) {
                    if (s->state == PLAYER_STATE_PLAYING) player_pause();
                    else player_play();
                } else if (i == 2) {
                    int next = player_mgr.active + 1;
                    while (next < PLAYER_MAX_SESSIONS && !player_mgr.sessions[next].used) next++;
                    if (next < PLAYER_MAX_SESSIONS) {
                        player_mgr.active = next;
                        gui_dirty = 1;
                    }
                } else {
                    player_stop();
                }
                gui_dirty = 1;
                return;
            }
        }
    }

    int vol_label_w = 36;
    int vol_y = btn_y + btn_h + 12;
    int vol_bar_x = x + 24 + vol_label_w;
    int vol_bar_w = w - 48 - vol_label_w - 100;
    if (my >= vol_y + 4 && my < vol_y + 20 && mx >= vol_bar_x && mx < vol_bar_x + vol_bar_w) {
        player_mgr.dragging_volume = 1;
        player_mgr.dragging_seek = 0;
        int rel = mx - vol_bar_x;
        if (rel < 0) rel = 0;
        if (rel > vol_bar_w) rel = vol_bar_w;
        int vol = rel * 100 / vol_bar_w;
        player_set_volume(vol);
        return;
    }

    int pl_toggle_x = x + w - 24 - FONT_WIDTH * 5;
    if (my >= vol_y && my < vol_y + 18 && mx >= pl_toggle_x && mx < pl_toggle_x + FONT_WIDTH * 5) {
        player_mgr.playlist_expanded = !player_mgr.playlist_expanded;
        gui_dirty = 1;
        return;
    }

    if (player_mgr.playlist_expanded) {
        int pl_y = vol_y + 24;
        int row_h = 20;
        if (my >= pl_y + 4 && mx >= x + 12 && mx < x + w - 12) {
            int idx = (my - (pl_y + 4)) / row_h;
            if (idx >= 0 && idx < player_mgr.playlist_count) {
                player_mgr.playlist_expanded = 0;
                gui_dirty = 1;
            }
        }
    }

    gui_dirty = 1;
}

void player_open_window(void) {
    player_mgr.visible = 1;
    gui_dirty = 1;
}
