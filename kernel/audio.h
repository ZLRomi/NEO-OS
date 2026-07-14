#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

#define AUDIO_SAMPLE_RATE   48000
#define AUDIO_CHANNELS      2
#define AUDIO_BITS_PER_SAMPLE 16
#define AUDIO_BYTES_PER_SAMPLE 2
#define AUDIO_FRAME_BYTES  (AUDIO_CHANNELS * AUDIO_BYTES_PER_SAMPLE)

#define AUDIO_BUFFER_FRAMES  1024
#define AUDIO_BUFFER_BYTES   (AUDIO_BUFFER_FRAMES * AUDIO_FRAME_BYTES)
#define AUDIO_NUM_BUFFERS    32

#define AUDIO_STATE_IDLE     0
#define AUDIO_STATE_PLAYING 1
#define AUDIO_STATE_PAUSED  2
#define AUDIO_STATE_STOPPED 3

typedef struct {
    int used;
    int state;
    uint32_t sample_rate;
    int channels;
    const void* pcm_data;
    uint32_t pcm_total_bytes;
    uint32_t pcm_pos;
    int loop;
    void (*on_complete)(void* ctx);
    void* ctx;
} audio_stream_t;

#define AUDIO_MAX_STREAMS 4

void audio_init(void);
int  audio_is_present(void);

int  audio_play(const void* pcm_data, uint32_t bytes, uint32_t sample_rate,
                int channels, int loop, void (*on_complete)(void*), void* ctx);
void audio_stop(int handle);
void audio_pause(int handle);
void audio_resume(int handle);
void audio_set_volume(int handle, int volume);
void audio_poll(void);

int  audio_get_volume(void);
void audio_set_master_volume(int volume);

uint32_t audio_get_position_ms(int handle);
uint32_t audio_get_duration_ms(int handle);

#endif
