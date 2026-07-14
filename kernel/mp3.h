#ifndef MP3_H
#define MP3_H

#include <stdint.h>

#define MP3_MAX_FRAME_SIZE  2304
#define MP3_PCM_SAMPLES    2304

#define MP3_ERR_NONE       0
#define MP3_ERR_SYNC       -1
#define MP3_ERR_HEADER     -2
#define MP3_ERR_CRC        -3
#define MP3_ERR_UNSUPPORTED -4
#define MP3_ERR_EOF        -5

#define MP3_LAYER_1  1
#define MP3_LAYER_2  2
#define MP3_LAYER_3  3

#define MP3_VERSION_2_5 0
#define MP3_VERSION_2   2
#define MP3_VERSION_1   3

#define MP3_MODE_STEREO       0
#define MP3_MODE_JOINT_STEREO 1
#define MP3_MODE_DUAL_CHANNEL 2
#define MP3_MODE_MONO         3

typedef struct {
    int version;
    int layer;
    int has_crc;
    int bitrate_kbps;
    int sample_rate_hz;
    int padding;
    int frame_length;
    int samples_per_frame;
    int side_info_size;
    int channel_mode;
    int channels;
    int mode_extension;
    int emphasis;
    int copyright;
    int original;
    int private_bit;
} mp3_frame_info_t;

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
    mp3_frame_info_t last_frame;
    int decoded_ok;
    int total_frames;
    uint32_t duration_ms;
} mp3_decoder_t;

void mp3_decoder_init(mp3_decoder_t* dec, const void* data, uint32_t size);
int  mp3_find_sync(mp3_decoder_t* dec);
int  mp3_parse_header(const uint8_t* hdr, mp3_frame_info_t* info);
int  mp3_decode_frame(mp3_decoder_t* dec, int16_t* pcm_out);
int  mp3_get_sample_rate(mp3_decoder_t* dec);
int  mp3_get_channels(mp3_decoder_t* dec);
uint32_t mp3_estimate_duration(const void* data, uint32_t size);
int  mp3_decode_all(const void* data, uint32_t size,
                    int16_t* pcm_out, uint32_t pcm_max,
                    int* sample_rate, int* channels);

#endif
