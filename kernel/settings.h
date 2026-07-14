#ifndef SETTINGS_H
#define SETTINGS_H

#include "gui.h"

void settings_init(void);
void render_settings(window_t* win);
void settings_handle_click(window_t* win, int mx, int my);

#endif
