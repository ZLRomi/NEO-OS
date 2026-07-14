#ifndef MP4_H
#define MP4_H

#include <stdint.h>

#define MP4_MAX_BOXES 64
#define MP4_MAX_TRACKS 8
#define MP4_MAX_FRAMES 1024
#define MP4_MAX_VIDEO_W 1920
#define MP4_MAX_VIDEO_H 1080

#define MP4_TRACK_TYPE_NONE  0
#define MP4_TRACK_TYPE_VIDEO 1
#define MP4_TRACK_TYPE_AUDIO 2

typedef struct {
    uint32_t offset;
    uint32_t size;
    uint64_t dts;
    uint64_t pts;
    int is_keyframe;
} mp4_sample_t;

typedef struct {
    int type;
    int track_id;
    int timescale;
    int duration;
    int width;
    int height;

    int sample_count;
    mp4_sample_t samples[MP4_MAX_FRAMES];

    uint8_t codec[8];
    int sample_rate;
    int channels;
} mp4_track_t;

typedef struct {
    const uint8_t* data;
    uint32_t size;
    mp4_track_t tracks[MP4_MAX_TRACKS];
    int track_count;
    int timescale;
    int duration_ms;
} mp4_parser_t;

void mp4_parser_init(mp4_parser_t* p, const void* data, uint32_t size);
int  mp4_parse(mp4_parser_t* p);
int  mp4_get_video_track(mp4_parser_t* p, mp4_track_t** out);
int  mp4_get_audio_track(mp4_parser_t* p, mp4_track_t** out);
uint32_t mp4_get_duration_ms(mp4_parser_t* p);

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
    int width;
    int height;
    int frame_index;
    int frame_count;
} mp4_video_decoder_t;

void mp4_video_decoder_init(mp4_video_decoder_t* dec, mp4_track_t* track,
                            const void* file_data, uint32_t file_size);
int  mp4_video_decode_frame(mp4_video_decoder_t* dec, uint32_t* rgb_out,
                             int out_w, int out_h);

#endif
