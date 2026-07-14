#include "theme.h"
#include "fat32.h"
#include "gui_internal.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define THEME_CONF_PATH "/etc/theme.conf"
#define THEME_BUF_SIZE 512

static theme_t g_theme_light = {
    0x000078D4, 0x00106EBE, 0x00F3F3F3, 0x001F1F1F, 0x00767676,
    0x00E5E5E5, 0x00FFFFFF, 0x00EAEAEA, 8, 2, 14
};

static theme_t g_theme_dark = {
    0x0060CDFF, 0x0078D4FF, 0x001F1F1F, 0x00FFFFFF, 0x00C5C5C5,
    0x00323232, 0x002B2B2B, 0x00202020, 8, 2, 14
};

theme_t g_theme;
static int g_theme_mode = THEME_MODE_LIGHT;

static int my_strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int parse_hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t parse_hex(const char* s) {
    uint32_t v = 0;
    int i = 0;
    while (s[i] && i < 8) {
        int d = parse_hex_digit(s[i]);
        if (d < 0) break;
        v = (v << 4) | (uint32_t)d;
        i++;
    }
    return v;
}

static int parse_int(const char* s) {
    int sign = 1;
    int i = 0;
    if (s[i] == '-') { sign = -1; i++; }
    int v = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10 + (s[i] - '0');
        i++;
    }
    return v * sign;
}

static void put_char(char** p, char c) {
    **p = c;
    (*p)++;
}

static void put_str(char** p, const char* s) {
    while (*s) {
        **p = *s;
        (*p)++;
        s++;
    }
}

static void put_hex6(char** p, uint32_t v) {
    const char* hex = "0123456789ABCDEF";
    int i;
    for (i = 5; i >= 0; i--) {
        put_char(p, hex[(v >> (i * 4)) & 0xF]);
    }
}

static void put_int(char** p, int v) {
    char tmp[12];
    int i = 0;
    int neg = 0;
    uint32_t u;
    if (v < 0) { neg = 1; u = (uint32_t)(-(int32_t)v); }
    else u = (uint32_t)v;
    if (u == 0) tmp[i++] = '0';
    while (u > 0) {
        tmp[i++] = '0' + (u % 10);
        u /= 10;
    }
    if (neg) put_char(p, '-');
    while (i > 0) put_char(p, tmp[--i]);
}

static void put_hex_line(char** p, const char* key, uint32_t v) {
    put_str(p, key);
    put_char(p, '=');
    put_hex6(p, v);
    put_char(p, '\n');
}

static void put_int_line(char** p, const char* key, int v) {
    put_str(p, key);
    put_char(p, '=');
    put_int(p, v);
    put_char(p, '\n');
}

static const char* get_value(const char* line, const char* key) {
    int k = 0;
    while (key[k] && line[k] == key[k]) k++;
    if (key[k] != '\0' || line[k] != '=') return NULL;
    return line + k + 1;
}

static void apply_color_line(const char* line, theme_t* t) {
    const char* v;
    if ((v = get_value(line, "accent")))             t->accent = parse_hex(v);
    else if ((v = get_value(line, "accent_hover")))  t->accent_hover = parse_hex(v);
    else if ((v = get_value(line, "bg")))              t->bg = parse_hex(v);
    else if ((v = get_value(line, "text")))           t->text = parse_hex(v);
    else if ((v = get_value(line, "text_secondary"))) t->text_secondary = parse_hex(v);
    else if ((v = get_value(line, "border")))         t->border = parse_hex(v);
    else if ((v = get_value(line, "title_bar")))      t->title_bar = parse_hex(v);
    else if ((v = get_value(line, "title_bar_bottom"))) t->title_bar_bottom = parse_hex(v);
    else if ((v = get_value(line, "corner_radius")))  t->corner_radius = parse_int(v);
    else if ((v = get_value(line, "shadow_offset")))  t->shadow_offset = parse_int(v);
    else if ((v = get_value(line, "font_size")))      t->font_size = parse_int(v);
}

static int find_mode(const char* buf, int total) {
    int mode = THEME_MODE_LIGHT;
    int start = 0;
    int i = 0;
    while (i <= total) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            char line[64];
            int o = 0;
            int s = start;
            while (s < i && buf[s] == '\r') s++;
            while (s < i && buf[s] != '\r' && o < 63) {
                line[o++] = buf[s++];
            }
            line[o] = '\0';
            if (line[0] != '\0' && line[0] != '#') {
                const char* v = get_value(line, "mode");
                if (v) {
                    if (my_strcmp(v, "dark") == 0) mode = THEME_MODE_DARK;
                    else mode = THEME_MODE_LIGHT;
                }
            }
            if (buf[i] == '\0') break;
            start = i + 1;
        }
        i++;
    }
    return mode;
}

void theme_init(void) {
    g_theme = g_theme_light;
    g_theme_mode = THEME_MODE_LIGHT;
    if (!fat32_is_mounted()) return;
    theme_load();
}

int theme_load(void) {
    if (!fat32_is_mounted()) return -1;

    int fd = fat32_open(THEME_CONF_PATH, 0);
    if (fd < 0) return -1;

    static char buf[THEME_BUF_SIZE];
    int total = 0;
    int n;
    while (total < THEME_BUF_SIZE - 1 &&
           (n = fat32_read(fd, buf + total, THEME_BUF_SIZE - 1 - total)) > 0) {
        total += n;
    }
    fat32_close(fd);

    buf[total] = '\0';

    int mode = find_mode(buf, total);
    g_theme_mode = mode;
    g_theme = (mode == THEME_MODE_DARK) ? g_theme_dark : g_theme_light;

    int start = 0;
    int i = 0;
    while (i <= total) {
        if (buf[i] == '\n' || buf[i] == '\0') {
            char line[64];
            int o = 0;
            int s = start;
            while (s < i && buf[s] == '\r') s++;
            while (s < i && buf[s] != '\r' && o < 63) {
                line[o++] = buf[s++];
            }
            line[o] = '\0';
            if (line[0] != '\0' && line[0] != '#' && !get_value(line, "mode")) {
                apply_color_line(line, &g_theme);
            }
            if (buf[i] == '\0') break;
            start = i + 1;
        }
        i++;
    }

    serial_write("theme: loaded from ");
    serial_write(THEME_CONF_PATH);
    serial_write("\n");
    return 0;
}

int theme_save(void) {
    if (!fat32_is_mounted()) return -1;

    int dh = fat32_opendir("/etc");
    if (dh >= 0) {
        fat32_close(dh);
    } else {
        fat32_mkdir("/etc");
    }

    fat32_remove(THEME_CONF_PATH);

    int fd = fat32_open(THEME_CONF_PATH, 1);
    if (fd < 0) {
        serial_write("theme: cannot create config\n");
        return -1;
    }

    static char buf[THEME_BUF_SIZE];
    char* p = buf;
    put_str(&p, "mode=");
    put_str(&p, (g_theme_mode == THEME_MODE_DARK) ? "dark" : "light");
    put_char(&p, '\n');
    put_hex_line(&p, "accent", g_theme.accent);
    put_hex_line(&p, "accent_hover", g_theme.accent_hover);
    put_hex_line(&p, "bg", g_theme.bg);
    put_hex_line(&p, "text", g_theme.text);
    put_hex_line(&p, "text_secondary", g_theme.text_secondary);
    put_hex_line(&p, "border", g_theme.border);
    put_hex_line(&p, "title_bar", g_theme.title_bar);
    put_hex_line(&p, "title_bar_bottom", g_theme.title_bar_bottom);
    put_int_line(&p, "corner_radius", g_theme.corner_radius);
    put_int_line(&p, "shadow_offset", g_theme.shadow_offset);
    put_int_line(&p, "font_size", g_theme.font_size);

    int len = (int)(p - buf);
    int written = fat32_write(fd, buf, len);
    fat32_close(fd);

    if (written != len) {
        serial_write("theme: short write\n");
        return -1;
    }
    serial_write("theme: saved to ");
    serial_write(THEME_CONF_PATH);
    serial_write("\n");
    return 0;
}

void theme_apply(theme_t* t) {
    if (t) {
        g_theme = *t;
    }
    gui_dirty = 1;
}

theme_t* theme_get_current(void) {
    return &g_theme;
}

void theme_set_light(void) {
    g_theme_mode = THEME_MODE_LIGHT;
    g_theme = g_theme_light;
    gui_dirty = 1;
    theme_save();
}

void theme_set_dark(void) {
    g_theme_mode = THEME_MODE_DARK;
    g_theme = g_theme_dark;
    gui_dirty = 1;
    theme_save();
}
