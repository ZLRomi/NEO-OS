#ifndef VIDEO_DECODE_IFACE_H
#define VIDEO_DECODE_IFACE_H

#include <stdint.h>

#define VIDEO_DECODE_OK      0
#define VIDEO_DECODE_ERR     -1
#define VIDEO_DECODE_EOS     -2
#define VIDEO_DECODE_NEED_MORE -3

typedef struct video_decoder video_decoder_t;

struct video_decoder {
    int (*init)(video_decoder_t* self, int width, int height, uint32_t timescale);
    int (*decode)(video_decoder_t* self, const void* frame_data, uint32_t frame_size,
                  uint32_t* rgb_out, int out_w, int out_h);
    int (*flush)(video_decoder_t* self);
    void (*destroy)(video_decoder_t* self);
    void* ctx;
    int width;
    int height;
    int frame_count;
};

typedef video_decoder_t* (*video_decoder_create_fn)(void);
void video_decoder_register(video_decoder_create_fn fn);
video_decoder_t* video_decoder_create(void);

#endif
