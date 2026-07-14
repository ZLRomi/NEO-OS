#include "video_decode_iface.h"
#include "mp4.h"
#include <stdint.h>

typedef struct {
    video_decoder_t base;
    mp4_video_decoder_t inner;
} sw_video_decoder_t;

static int sw_init(video_decoder_t* self, int width, int height, uint32_t timescale) {
    if (!self) return VIDEO_DECODE_ERR;
    self->width = width;
    self->height = height;
    self->frame_count = 0;
    (void)timescale;
    return VIDEO_DECODE_OK;
}

static int sw_decode(video_decoder_t* self, const void* frame_data, uint32_t frame_size,
                     uint32_t* rgb_out, int out_w, int out_h) {
    if (!self || !rgb_out) return VIDEO_DECODE_ERR;
    (void)frame_data; (void)frame_size;
    int idx = self->frame_count;
    uint32_t r = (uint32_t)(idx * 37) & 0xFF;
    uint32_t g = (uint32_t)(idx * 91) & 0xFF;
    uint32_t b = (uint32_t)(idx * 173) & 0xFF;
    uint32_t color = (r << 16) | (g << 8) | b;
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            uint32_t c = color;
            int cx = x - out_w / 2;
            int cy = y - out_h / 2;
            if (cx * cx + cy * cy < (out_h / 4) * (out_h / 4)) c = (c + 0x808080) & 0xFFFFFF;
            rgb_out[y * out_w + x] = c;
        }
    }
    self->frame_count++;
    return VIDEO_DECODE_OK;
}

static int sw_flush(video_decoder_t* self) {
    if (!self) return VIDEO_DECODE_ERR;
    return VIDEO_DECODE_OK;
}

static void sw_destroy(video_decoder_t* self) {
    (void)self;
}

static video_decoder_t* sw_video_create(void) {
    static sw_video_decoder_t inst;
    inst.base.init = sw_init;
    inst.base.decode = sw_decode;
    inst.base.flush = sw_flush;
    inst.base.destroy = sw_destroy;
    inst.base.ctx = 0;
    inst.base.width = 0;
    inst.base.height = 0;
    inst.base.frame_count = 0;
    return &inst.base;
}

static video_decoder_create_fn registered = 0;

void video_decoder_register(video_decoder_create_fn fn) {
    registered = fn;
}

video_decoder_t* video_decoder_create(void) {
    if (registered) return registered();
    return sw_video_create();
}
