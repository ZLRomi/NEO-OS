#ifndef DOWNLOAD_H
#define DOWNLOAD_H

#include <stdint.h>
#include "gui.h"

#define DL_MAX_TASKS 8
#define DL_URL_SIZE  256
#define DL_NAME_SIZE 64
#define DL_PATH_SIZE 96
#define DL_BUF_SIZE  4096

#define DL_STATE_IDLE     0
#define DL_STATE_RUNNING  1
#define DL_STATE_DONE     2
#define DL_STATE_FAILED   3
#define DL_STATE_CANCELED 4

typedef struct {
    int used;
    int state;
    char url[DL_URL_SIZE];
    char filename[DL_NAME_SIZE];
    char save_path[DL_PATH_SIZE];
    int total_bytes;
    int received_bytes;
    int progress;
    int prev_bytes;
    int speed_kbs;
    int last_speed_tick;
} dl_task_t;

typedef struct {
    dl_task_t tasks[DL_MAX_TASKS];
    int count;
    int visible;
    int selected;
    int ctx_menu_visible;
    int ctx_menu_x;
    int ctx_menu_y;
    int ctx_menu_idx;
} dl_manager_t;

extern dl_manager_t dl_mgr;

void download_init(void);
int download_start(const char* url, const char* save_dir);
void download_cancel(int idx);
void download_remove(int idx);
int download_poll(void);

void render_downloads(window_t* win);
void downloads_handle_click(window_t* win, int mx, int my);
void downloads_open(void);

#endif
