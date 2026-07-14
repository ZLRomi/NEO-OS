#ifndef PLAYER_H
#define PLAYER_H

#include "gui.h"

#define PLAYER_URL_SIZE 256
#define PLAYER_PCM_MAX (1152 * 2 * 256)

typedef enum {
    PLAYER_MODE_NONE = 0,
    PLAYER_MODE_AUDIO,
    PLAYER_MODE_VIDEO,
} player_mode_t;

typedef enum {
    PLAYER_STATE_IDLE = 0,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_PAUSED,
    PLAYER_STATE_STOPPED,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef struct {
    int used;
    player_mode_t mode;
    player_state_t state;
    char filepath[PLAYER_URL_SIZE];
    char filename[64];
    int duration_ms;
    int position_ms;
    int sample_rate;
    int channels;
    int volume;
    int video_width;
    int video_height;
    int video_frame_idx;
    int video_frame_count;
    int has_pcm;
    int16_t* pcm_data;
    uint32_t pcm_samples;
    int audio_handle;
} player_session_t;

#define PLAYER_MAX_SESSIONS 2
#define PLAYER_PLAYLIST_SIZE 8
#define PLAYER_PLAYLIST_NAME 64

typedef struct {
    player_session_t sessions[PLAYER_MAX_SESSIONS];
    int active;
    int visible;
    char playlist[PLAYER_PLAYLIST_SIZE][PLAYER_PLAYLIST_NAME];
    int playlist_count;
    int playlist_expanded;
    int rotation;
    int dragging_seek;
    int dragging_volume;
} player_manager_t;

extern player_manager_t player_mgr;

void player_init(void);
int  player_open(const char* path);
void player_play(void);
void player_pause(void);
void player_stop(void);
void player_seek(int pos_ms);
void player_set_volume(int vol);
void player_poll(void);

void render_player(window_t* win);
void player_handle_click(window_t* win, int mx, int my);
void player_open_window(void);

int  player_is_supported(const char* filename);

#endif
