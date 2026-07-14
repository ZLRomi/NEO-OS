#include "mp4.h"
#include "serial.h"
#include <stdint.h>

static uint32_t read_be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t read_be64(const uint8_t* p) {
    return ((uint64_t)read_be32(p) << 32) | (uint64_t)read_be32(p + 4);
}

static uint16_t read_be16(const uint8_t* p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static int fourcc_eq(const uint8_t* a, const char* b) {
    for (int i = 0; i < 4; i++) {
        if ((char)a[i] != b[i]) return 0;
    }
    return 1;
}

static void copy_fourcc(char* dst, const uint8_t* src) {
    for (int i = 0; i < 4; i++) dst[i] = (char)src[i];
    dst[4] = '\0';
}

void mp4_parser_init(mp4_parser_t* p, const void* data, uint32_t size) {
    if (!p) return;
    p->data = (const uint8_t*)data;
    p->size = size;
    p->track_count = 0;
    p->timescale = 0;
    p->duration_ms = 0;
    for (int i = 0; i < MP4_MAX_TRACKS; i++) {
        p->tracks[i].type = MP4_TRACK_TYPE_NONE;
        p->tracks[i].track_id = i + 1;
        p->tracks[i].sample_count = 0;
    }
}

static int alloc_track(mp4_parser_t* p) {
    if (p->track_count >= MP4_MAX_TRACKS) return -1;
    return p->track_count++;
}

static int parse_stsz(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 12) return 0;
    uint32_t version = read_be32(box);
    (void)version;
    uint32_t sample_size = read_be32(box + 4);
    uint32_t count = read_be32(box + 8);
    (void)sample_size;
    if (count > MP4_MAX_FRAMES) count = MP4_MAX_FRAMES;
    track->sample_count = (int)count;
    if (sample_size != 0) {
        for (uint32_t i = 0; i < count; i++) {
            track->samples[i].size = sample_size;
        }
        return 1;
    }
    if (box_size < 12 + count * 4) return 0;
    for (uint32_t i = 0; i < count; i++) {
        track->samples[i].size = read_be32(box + 12 + i * 4);
    }
    return 1;
}

static int parse_stco(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 8) return 0;
    uint32_t count = read_be32(box + 4);
    if (count > MP4_MAX_FRAMES) count = MP4_MAX_FRAMES;
    if (box_size < 8 + count * 4) return 0;
    for (uint32_t i = 0; i < count; i++) {
        track->samples[i].offset = read_be32(box + 8 + i * 4);
    }
    return 1;
}

static int parse_stts(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 8) return 0;
    uint32_t count = read_be32(box + 4);
    if (count > 64) count = 64;
    uint32_t pos = 8;
    uint32_t sample_idx = 0;
    uint64_t dts = 0;
    for (uint32_t i = 0; i < count && pos + 8 <= box_size; i++) {
        uint32_t delta = read_be32(box + pos);
        uint32_t repeats = read_be32(box + pos + 4);
        pos += 8;
        for (uint32_t r = 0; r < repeats && sample_idx < MP4_MAX_FRAMES; r++) {
            track->samples[sample_idx].dts = dts;
            track->samples[sample_idx].pts = dts;
            dts += delta;
            sample_idx++;
        }
    }
    return 1;
}

static int parse_stss(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 8) return 0;
    uint32_t count = read_be32(box + 4);
    if (count > MP4_MAX_FRAMES) count = MP4_MAX_FRAMES;
    if (box_size < 8 + count * 4) return 0;
    for (int i = 0; i < track->sample_count; i++) {
        track->samples[i].is_keyframe = 0;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = read_be32(box + 8 + i * 4);
        if (idx > 0 && idx <= (uint32_t)track->sample_count) {
            track->samples[idx - 1].is_keyframe = 1;
        }
    }
    return 1;
}

static int parse_hdlr(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 16) return 0;
    const uint8_t* h = box + 8;
    if (fourcc_eq(h, "vide")) track->type = MP4_TRACK_TYPE_VIDEO;
    else if (fourcc_eq(h, "soun")) track->type = MP4_TRACK_TYPE_AUDIO;
    return 1;
}

static int parse_mdhd(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 24) return 0;
    uint32_t version = read_be32(box);
    if (version == 0) {
        track->timescale = (int)read_be32(box + 12);
        track->duration = (int)read_be32(box + 16);
    } else {
        if (box_size < 36) return 0;
        track->timescale = (int)read_be32(box + 20);
        track->duration = (int)read_be64(box + 24);
    }
    return 1;
}

static int parse_tkhd(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 12) return 0;
    uint32_t version = read_be32(box);
    int off = (version == 0) ? 12 : 20;
    if (box_size < (uint32_t)(off + 4)) return 0;
    track->track_id = (int)read_be32(box + off);
    return 1;
}

static void parse_stsd_codec(const uint8_t* box, uint32_t box_size, mp4_track_t* track) {
    if (box_size < 24) return;
    const uint8_t* entry = box + 16;
    copy_fourcc((char*)track->codec, entry);
    if (track->type == MP4_TRACK_TYPE_VIDEO && box_size >= 40) {
        track->width = (int)read_be16(entry + 24);
        track->height = (int)read_be16(entry + 26);
    } else if (track->type == MP4_TRACK_TYPE_AUDIO && box_size >= 36) {
        int chan = (int)read_be16(entry + 16);
        int sr = (int)read_be32(entry + 24);
        track->channels = chan;
        track->sample_rate = sr;
    }
}

static int parse_box(mp4_parser_t* p, uint32_t start, uint32_t end,
                     int* track_idx, const char* parent);

static int parse_stbl(mp4_parser_t* p, uint32_t start, uint32_t end, int track_idx) {
    uint32_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_be32(p->data + pos);
        const uint8_t* type = p->data + pos + 4;
        if (size < 8) break;
        if (pos + size > end) break;
        if (fourcc_eq(type, "stsz")) parse_stsz(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "stco")) parse_stco(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "stts")) parse_stts(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "stss")) parse_stss(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "stsd")) parse_stsd_codec(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        pos += size;
    }
    return 1;
}

static int parse_minf(mp4_parser_t* p, uint32_t start, uint32_t end, int track_idx) {
    uint32_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_be32(p->data + pos);
        const uint8_t* type = p->data + pos + 4;
        if (size < 8) break;
        if (pos + size > end) break;
        if (fourcc_eq(type, "stbl")) {
            parse_stbl(p, pos + 8, pos + size, track_idx);
        }
        pos += size;
    }
    return 1;
}

static int parse_mdia(mp4_parser_t* p, uint32_t start, uint32_t end, int track_idx) {
    uint32_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_be32(p->data + pos);
        const uint8_t* type = p->data + pos + 4;
        if (size < 8) break;
        if (pos + size > end) break;
        if (fourcc_eq(type, "mdhd")) parse_mdhd(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "hdlr")) parse_hdlr(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "minf")) parse_minf(p, pos + 8, pos + size, track_idx);
        pos += size;
    }
    return 1;
}

static int parse_trak(mp4_parser_t* p, uint32_t start, uint32_t end) {
    int track_idx = alloc_track(p);
    if (track_idx < 0) return 0;
    uint32_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_be32(p->data + pos);
        const uint8_t* type = p->data + pos + 4;
        if (size < 8) break;
        if (pos + size > end) break;
        if (fourcc_eq(type, "tkhd")) parse_tkhd(p->data + pos + 8, size - 8, &p->tracks[track_idx]);
        else if (fourcc_eq(type, "mdia")) parse_mdia(p, pos + 8, pos + size, track_idx);
        pos += size;
    }
    return 1;
}

static int parse_mvhd(mp4_parser_t* p, uint32_t start, uint32_t end) {
    if (end - start < 24) return 0;
    uint32_t version = read_be32(p->data + start);
    int off = (version == 0) ? 12 : 20;
    if ((int)(end - start) < off + 8) return 0;
    p->timescale = (int)read_be32(p->data + start + off);
    if (version == 0) {
        uint32_t dur = read_be32(p->data + start + off + 4);
        if (p->timescale > 0) p->duration_ms = (int)(dur / (uint32_t)p->timescale * 1000);
    } else {
        uint64_t dur = read_be64(p->data + start + off + 4);
        uint32_t dur_lo = (uint32_t)dur;
        if (p->timescale > 0) p->duration_ms = (int)(dur_lo / (uint32_t)p->timescale * 1000);
    }
    return 1;
}

static int parse_box(mp4_parser_t* p, uint32_t start, uint32_t end,
                     int* track_idx, const char* parent) {
    (void)track_idx;
    (void)parent;
    uint32_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = read_be32(p->data + pos);
        const uint8_t* type = p->data + pos + 4;
        if (size < 8) break;
        if (pos + size > end) break;
        if (fourcc_eq(type, "moov")) {
            parse_box(p, pos + 8, pos + size, track_idx, "moov");
        } else if (fourcc_eq(type, "trak")) {
            parse_trak(p, pos + 8, pos + size);
        } else if (fourcc_eq(type, "mvhd")) {
            parse_mvhd(p, pos + 8, pos + size);
        }
        pos += size;
    }
    return 1;
}

int mp4_parse(mp4_parser_t* p) {
    if (!p || !p->data || p->size < 8) return 0;
    int track_idx = -1;
    return parse_box(p, 0, p->size, &track_idx, "root");
}

int mp4_get_video_track(mp4_parser_t* p, mp4_track_t** out) {
    if (!p || !out) return 0;
    for (int i = 0; i < p->track_count; i++) {
        if (p->tracks[i].type == MP4_TRACK_TYPE_VIDEO) {
            *out = &p->tracks[i];
            return 1;
        }
    }
    return 0;
}

int mp4_get_audio_track(mp4_parser_t* p, mp4_track_t** out) {
    if (!p || !out) return 0;
    for (int i = 0; i < p->track_count; i++) {
        if (p->tracks[i].type == MP4_TRACK_TYPE_AUDIO) {
            *out = &p->tracks[i];
            return 1;
        }
    }
    return 0;
}

uint32_t mp4_get_duration_ms(mp4_parser_t* p) {
    if (!p) return 0;
    return (uint32_t)p->duration_ms;
}

void mp4_video_decoder_init(mp4_video_decoder_t* dec, mp4_track_t* track,
                            const void* file_data, uint32_t file_size) {
    if (!dec) return;
    dec->data = (const uint8_t*)file_data;
    dec->size = file_size;
    dec->pos = 0;
    dec->width = track ? track->width : 0;
    dec->height = track ? track->height : 0;
    dec->frame_index = 0;
    dec->frame_count = track ? track->sample_count : 0;
}

int mp4_video_decode_frame(mp4_video_decoder_t* dec, uint32_t* rgb_out,
                            int out_w, int out_h) {
    if (!dec || !rgb_out) return 0;
    int frame = dec->frame_index;
    if (frame >= dec->frame_count) return 0;
    int fx = 0, fy = 0;
    int fw = dec->width ? dec->width : out_w;
    int fh = dec->height ? dec->height : out_h;
    if (fw > out_w) fw = out_w;
    if (fh > out_h) fh = out_h;
    uint32_t r = (uint32_t)(frame * 37) & 0xFF;
    uint32_t g = (uint32_t)(frame * 91) & 0xFF;
    uint32_t b = (uint32_t)(frame * 173) & 0xFF;
    uint32_t color = (r << 16) | (g << 8) | b;
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            int cx = x - out_w / 2;
            int cy = y - out_h / 2;
            uint32_t c = color;
            if (cx * cx + cy * cy < (out_h / 4) * (out_h / 4)) {
                c = (c + 0x808080) & 0xFFFFFF;
            }
            rgb_out[y * out_w + x] = c;
        }
    }
    (void)fx; (void)fy; (void)fw; (void)fh;
    dec->frame_index++;
    return 1;
}
