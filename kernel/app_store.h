#ifndef APP_STORE_H
#define APP_STORE_H

#include "gui.h"

void app_store_init(void);
void app_store_open(void);
void app_store_render(window_t* win);
int app_store_handle_click(window_t* win, int mx, int my);

#endif
