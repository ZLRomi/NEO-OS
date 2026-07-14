#ifndef AUDIO_DECODE_IFACE_H
#define AUDIO_DECODE_IFACE_H

#include <stdint.h>

#define AUDIO_DECODE_OK      0
#define AUDIO_DECODE_ERR     -1
#define AUDIO_DECODE_EOS     -2
#define AUDIO_DECODE_NEED_MORE -3

typedef struct audio_decoder audio_decoder_t;

struct audio_decoder {
    int (*init)(audio_decoder_t* self, int sample_rate, int channels);
    int (*decode)(audio_decoder_t* self, const void* frame_data, uint32_t frame_size,
                  int16_t* pcm_out, int pcm_max);
    int (*flush)(audio_decoder_t* self);
    void (*destroy)(audio_decoder_t* self);
    void* ctx;
    int sample_rate;
    int channels;
    int frame_count;
};

typedef audio_decoder_t* (*audio_decoder_create_fn)(void);
void audio_decoder_register(audio_decoder_create_fn fn);
audio_decoder_t* audio_decoder_create(void);

#endif
