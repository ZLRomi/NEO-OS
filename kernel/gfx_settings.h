#ifndef GFX_SETTINGS_H
#define GFX_SETTINGS_H

void gfx_settings_open(void);
void gfx_settings_close(void);
int gfx_settings_is_open(void);
void gfx_settings_render(void);
int gfx_settings_handle_click(int x, int y);

#endif
