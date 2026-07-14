#include "audio_decode_iface.h"
#include "mp3.h"
#include <stdint.h>

typedef struct {
    audio_decoder_t base;
    mp3_decoder_t mp3;
} sw_audio_decoder_t;

static int saud_init(audio_decoder_t* self, int sample_rate, int channels) {
    if (!self) return AUDIO_DECODE_ERR;
    self->sample_rate = sample_rate ? sample_rate : 44100;
    self->channels = channels ? channels : 2;
    self->frame_count = 0;
    return AUDIO_DECODE_OK;
}

static int saud_decode(audio_decoder_t* self, const void* frame_data, uint32_t frame_size,
                       int16_t* pcm_out, int pcm_max) {
    if (!self || !pcm_out) return AUDIO_DECODE_ERR;
    (void)frame_data; (void)frame_size; (void)pcm_max;
    for (int i = 0; i < 1152; i++) pcm_out[i] = 0;
    self->frame_count++;
    return AUDIO_DECODE_OK;
}

static int saud_flush(audio_decoder_t* self) {
    if (!self) return AUDIO_DECODE_ERR;
    return AUDIO_DECODE_OK;
}

static void saud_destroy(audio_decoder_t* self) {
    (void)self;
}

static audio_decoder_t* sw_audio_create(void) {
    static sw_audio_decoder_t inst;
    inst.base.init = saud_init;
    inst.base.decode = saud_decode;
    inst.base.flush = saud_flush;
    inst.base.destroy = saud_destroy;
    inst.base.ctx = 0;
    inst.base.sample_rate = 0;
    inst.base.channels = 0;
    inst.base.frame_count = 0;
    return &inst.base;
}

static audio_decoder_create_fn registered = 0;

void audio_decoder_register(audio_decoder_create_fn fn) {
    registered = fn;
}

audio_decoder_t* audio_decoder_create(void) {
    if (registered) return registered();
    return sw_audio_create();
}
