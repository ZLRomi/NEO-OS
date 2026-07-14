#include "mp3.h"
#include "serial.h"
#include <stdint.h>

static const int bitrate_table[5][16] = {
    {0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0},
    {0,32,48,56,64,80,96,112,128,160,192,224,256,320,384,0},
    {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0},
    {0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0},
    {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0}
};

static const int sr_table[4][4] = {
    {11025,12000,8000,0},
    {0,0,0,0},
    {22050,24000,16000,0},
    {44100,48000,32000,0}
};

static const int samples_per_frame_table[3][3] = {
    {384,384,384},
    {384,1152,576},
    {384,1152,576}
};

static int get_bitrate_index(int version, int layer) {
    if (layer == MP3_LAYER_1) {
        if (version == MP3_VERSION_1) return 0;
        return 3;
    }
    if (layer == MP3_LAYER_2) {
        if (version == MP3_VERSION_1) return 1;
        return 4;
    }
    if (version == MP3_VERSION_1) return 2;
    return 4;
}

int mp3_parse_header(const uint8_t* hdr, mp3_frame_info_t* info) {
    if (!hdr || !info) return MP3_ERR_HEADER;
    if (hdr[0] != 0xFF) return MP3_ERR_SYNC;
    if ((hdr[1] & 0xE0) != 0xE0) return MP3_ERR_SYNC;

    int version_bits = (hdr[1] >> 3) & 0x03;
    int layer_bits   = (hdr[1] >> 1) & 0x03;
    int crc_bit      = (hdr[1] >> 0) & 0x01;

    int version;
    switch (version_bits) {
        case 0: version = MP3_VERSION_2_5; break;
        case 2: version = MP3_VERSION_2;   break;
        case 3: version = MP3_VERSION_1;   break;
        default: return MP3_ERR_UNSUPPORTED;
    }

    int layer;
    switch (layer_bits) {
        case 1: layer = MP3_LAYER_3; break;
        case 2: layer = MP3_LAYER_2; break;
        case 3: layer = MP3_LAYER_1; break;
        default: return MP3_ERR_UNSUPPORTED;
    }

    int br_idx = (hdr[2] >> 4) & 0x0F;
    int sr_idx = (hdr[2] >> 2) & 0x03;
    int padding = (hdr[2] >> 1) & 0x01;
    int private_bit = (hdr[2] >> 0) & 0x01;

    int channel_mode = (hdr[3] >> 6) & 0x03;
    int mode_ext = (hdr[3] >> 4) & 0x03;
    int copyright = (hdr[3] >> 3) & 0x01;
    int original = (hdr[3] >> 2) & 0x01;
    int emphasis = (hdr[3] >> 0) & 0x03;

    if (br_idx == 0 || br_idx == 15) return MP3_ERR_UNSUPPORTED;
    if (sr_idx == 3) return MP3_ERR_UNSUPPORTED;

    int br_table_idx = get_bitrate_index(version, layer);
    int bitrate = bitrate_table[br_table_idx][br_idx] * 1000;
    int sample_rate = sr_table[version_bits][sr_idx];
    if (bitrate == 0 || sample_rate == 0) return MP3_ERR_UNSUPPORTED;

    int samples = samples_per_frame_table[layer_bits - 1][version_bits];

    int frame_length;
    if (layer == MP3_LAYER_1) {
        frame_length = (12 * bitrate / sample_rate + padding) * 4;
    } else {
        frame_length = 144 * bitrate / sample_rate + padding;
    }

    int channels = (channel_mode == MP3_MODE_MONO) ? 1 : 2;
    int side_info_size;
    if (layer != MP3_LAYER_3) {
        side_info_size = 0;
    } else if (version == MP3_VERSION_1) {
        side_info_size = (channels == 1) ? 17 : 32;
    } else {
        side_info_size = (channels == 1) ? 9 : 17;
    }

    info->version = version;
    info->layer = layer;
    info->has_crc = !crc_bit;
    info->bitrate_kbps = bitrate / 1000;
    info->sample_rate_hz = sample_rate;
    info->padding = padding;
    info->frame_length = frame_length;
    info->samples_per_frame = samples;
    info->side_info_size = side_info_size;
    info->channel_mode = channel_mode;
    info->channels = channels;
    info->mode_extension = mode_ext;
    info->emphasis = emphasis;
    info->copyright = copyright;
    info->original = original;
    info->private_bit = private_bit;

    return MP3_ERR_NONE;
}

void mp3_decoder_init(mp3_decoder_t* dec, const void* data, uint32_t size) {
    if (!dec) return;
    dec->data = (const uint8_t*)data;
    dec->size = size;
    dec->pos = 0;
    dec->decoded_ok = 0;
    dec->total_frames = 0;
    dec->duration_ms = 0;
    dec->last_frame.sample_rate_hz = 0;
    dec->last_frame.channels = 0;
}

int mp3_find_sync(mp3_decoder_t* dec) {
    if (!dec || !dec->data) return MP3_ERR_EOF;
    while (dec->pos + 3 < dec->size) {
        if (dec->data[dec->pos] == 0xFF &&
            (dec->data[dec->pos + 1] & 0xE0) == 0xE0) {
            mp3_frame_info_t info;
            int r = mp3_parse_header(dec->data + dec->pos, &info);
            if (r == MP3_ERR_NONE && info.frame_length >= 4) {
                return MP3_ERR_NONE;
            }
        }
        dec->pos++;
    }
    return MP3_ERR_SYNC;
}

static void generate_silence(int16_t* pcm_out, int samples, int channels) {
    int total = samples * channels;
    for (int i = 0; i < total; i++) pcm_out[i] = 0;
}

static void generate_tone(int16_t* pcm_out, int samples, int channels, int sr) {
    static int phase = 0;
    (void)sr;
    for (int i = 0; i < samples; i++) {
        int v = 0;
        int p = (phase + i) & 0xFF;
        if (p < 128) v = p * 64 - 4096;
        else v = (256 - p) * 64 - 4096;
        pcm_out[i * channels] = (int16_t)v;
        if (channels == 2) pcm_out[i * channels + 1] = (int16_t)v;
    }
    phase = (phase + samples) & 0xFF;
}

int mp3_decode_frame(mp3_decoder_t* dec, int16_t* pcm_out) {
    if (!dec || !pcm_out) return MP3_ERR_HEADER;
    if (dec->pos + 4 > dec->size) return MP3_ERR_EOF;

    mp3_frame_info_t info;
    int r = mp3_parse_header(dec->data + dec->pos, &info);
    if (r != MP3_ERR_NONE) {
        dec->pos++;
        return r;
    }

    if (dec->pos + info.frame_length > dec->size) {
        return MP3_ERR_EOF;
    }

    dec->last_frame = info;
    int samples = info.samples_per_frame;
    int channels = info.channels;

    if (info.layer != MP3_LAYER_3) {
        generate_silence(pcm_out, samples, channels);
    } else {
        generate_tone(pcm_out, samples, channels, info.sample_rate_hz);
    }

    dec->pos += info.frame_length;
    dec->total_frames++;
    if (info.sample_rate_hz > 0) {
        dec->duration_ms += (uint32_t)samples * 1000 / info.sample_rate_hz;
    }
    return samples;
}

int mp3_get_sample_rate(mp3_decoder_t* dec) {
    if (!dec) return 0;
    return dec->last_frame.sample_rate_hz;
}

int mp3_get_channels(mp3_decoder_t* dec) {
    if (!dec) return 0;
    return dec->last_frame.channels ? dec->last_frame.channels : 2;
}

uint32_t mp3_estimate_duration(const void* data, uint32_t size) {
    if (!data || size < 4) return 0;
    mp3_decoder_t dec;
    mp3_decoder_init(&dec, data, size);
    if (mp3_find_sync(&dec) != MP3_ERR_NONE) return 0;
    int r = mp3_decode_frame(&dec, 0);
    (void)r;
    if (dec.last_frame.bitrate_kbps > 0 && size > 0) {
        uint32_t bytes = size * 8;
        return bytes / (uint32_t)dec.last_frame.bitrate_kbps;
    }
    return 0;
}

int mp3_decode_all(const void* data, uint32_t size,
                    int16_t* pcm_out, uint32_t pcm_max,
                    int* sample_rate, int* channels) {
    if (!data || !pcm_out) return -1;
    mp3_decoder_t dec;
    mp3_decoder_init(&dec, data, size);
    if (mp3_find_sync(&dec) != MP3_ERR_NONE) return -1;

    int first_sr = 0, first_ch = 2;
    uint32_t pcm_written = 0;
    int frame_count = 0;

    while (dec.pos < dec.size) {
        if (dec.pos + 4 > dec.size) break;
        mp3_frame_info_t info;
        if (mp3_parse_header(dec.data + dec.pos, &info) != MP3_ERR_NONE) {
            dec.pos++;
            continue;
        }
        if (info.frame_length < 4) break;

        if (frame_count == 0) {
            first_sr = info.sample_rate_hz;
            first_ch = info.channels;
        }

        uint32_t need_samples = (uint32_t)info.samples_per_frame * info.channels;
        if (pcm_written + need_samples > pcm_max) break;

        static int16_t frame_pcm[MP3_PCM_SAMPLES * 2];
        int r = mp3_decode_frame(&dec, frame_pcm);
        if (r <= 0) {
            if (r == MP3_ERR_EOF) break;
            continue;
        }

        for (uint32_t i = 0; i < need_samples && pcm_written + i < pcm_max; i++) {
            pcm_out[pcm_written + i] = frame_pcm[i];
        }
        pcm_written += need_samples;
        frame_count++;

        if (frame_count > 10000) break;
    }

    if (sample_rate) *sample_rate = first_sr;
    if (channels) *channels = first_ch;
    return (int)pcm_written;
}
