#ifndef THEME_H
#define THEME_H

#include <stdint.h>

typedef struct {
    uint32_t accent;
    uint32_t accent_hover;
    uint32_t bg;
    uint32_t text;
    uint32_t text_secondary;
    uint32_t border;
    uint32_t title_bar;
    uint32_t title_bar_bottom;
    int corner_radius;
    int shadow_offset;
    int font_size;
} theme_t;

extern theme_t g_theme;

#define THEME_MODE_LIGHT 0
#define THEME_MODE_DARK  1

void theme_init(void);
void theme_apply(theme_t* t);
int theme_load(void);
int theme_save(void);
theme_t* theme_get_current(void);
void theme_set_light(void);
void theme_set_dark(void);

#endif
