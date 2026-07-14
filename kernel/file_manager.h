#ifndef FILE_MANAGER_H
#define FILE_MANAGER_H

#include "gui.h"

void file_manager_init(void);
void render_file_manager(window_t* win);
void file_manager_handle_click(window_t* win, int mx, int my);

#endif
