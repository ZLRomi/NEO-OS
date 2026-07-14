#ifndef WALLPAPER_H
#define WALLPAPER_H

#define WALLPAPER_PATH_MAX 256

typedef enum {
    WALLPAPER_STRETCH = 0,
    WALLPAPER_CENTER  = 1,
    WALLPAPER_SCALE   = 2
} wallpaper_mode_t;

extern int g_wallpaper_loaded;
extern int g_wallpaper_mode;
extern char g_wallpaper_path[WALLPAPER_PATH_MAX];

void wallpaper_init(void);
int wallpaper_load(const char* path, int mode);
int wallpaper_load_embedded_default(void);
void wallpaper_render(void);
void wallpaper_save_config(void);

#endif
