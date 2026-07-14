#include "jpeg.h"
#include "fat32.h"
#include "heap.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define MAX_COMPS 4

static uint8_t* read_file_all(const char* path, uint32_t* out_size) {
    int fd = fat32_open(path, 0);
    if (fd < 0) return 0;
    uint32_t total = 0;
    {
        uint8_t tmp[1024];
        int n;
        while ((n = fat32_read(fd, tmp, sizeof(tmp))) > 0) total += (uint32_t)n;
    }
    fat32_close(fd);
    if (total == 0) return 0;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) return 0;
    fd = fat32_open(path, 0);
    if (fd < 0) { kfree(buf); return 0; }
    uint32_t got = 0;
    while (got < total) {
        int n = fat32_read(fd, buf + got, (int)(total - got));
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    fat32_close(fd);
    if (got != total) { kfree(buf); return 0; }
    *out_size = total;
    return buf;
}

static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

static const int icos[8][8] = {
    {11585,11585,11585,11585,11585,11585,11585,11585},
    {16069,13623, 9103, 3196,-3196,-9103,-13623,-16069},
    {15137, 6270,-6270,-15137,-15137,-6270, 6270,15137},
    {13623,-3196,-16069,-9103, 9103,16069, 3196,-13623},
    {11585,-11585,-11585,11585,11585,-11585,-11585,11585},
    { 9103,-16069, 3196,13623,-13623,-3196,16069,-9103},
    { 6270,-15137,15137,-6270,-6270,15137,-15137, 6270},
    { 3196,-9103,13623,-16069,16069,-13623, 9103,-3196}
};

typedef struct {
    int mincode[17];
    int maxcode[17];
    int valptr[17];
    uint8_t huffval[256];
    int nvals;
} huff_t;

typedef struct {
    int id;
    int h, v;
    int tq;
    int td, ta;
    int cw, ch;
    int16_t* out;
} comp_t;

typedef struct {
    const uint8_t* data;
    uint32_t size;
    uint32_t pos;
    uint32_t bb;
    int bits;
    int marker;
    int eoi;
    int width, height;
    int nf;
    int hmax, vmax;
    comp_t comp[MAX_COMPS];
    uint16_t qtab[MAX_COMPS][64];
    huff_t dc_tab[MAX_COMPS];
    huff_t ac_tab[MAX_COMPS];
    int restart_interval;
    int scan_ns;
    int scan_comp[MAX_COMPS];
    int fmcus_x, fmcus_y;
} jpg_t;

static int u16(const uint8_t* p) {
    return ((int)p[0] << 8) | (int)p[1];
}

static void build_huff(huff_t* t, const uint8_t* bits, const uint8_t* vals) {
    int p = 0;
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        int cnt = bits[len - 1];
        t->valptr[len] = p;
        t->mincode[len] = code;
        for (int i = 0; i < cnt; i++) {
            t->huffval[p] = vals[p];
            p++;
        }
        t->maxcode[len] = (cnt > 0) ? (code + cnt - 1) : -1;
        code = (code + cnt) << 1;
    }
    t->nvals = p;
}

static void fill_bits(jpg_t* j) {
    while (j->bits <= 16) {
        if (j->pos >= j->size) { j->eoi = 1; break; }
        uint8_t b = j->data[j->pos];
        if (b != 0xFF) {
            j->bb = (j->bb << 8) | b;
            j->bits += 8;
            j->pos++;
        } else {
            if (j->pos + 1 >= j->size) { j->eoi = 1; j->pos++; break; }
            uint8_t nb = j->data[j->pos + 1];
            if (nb == 0x00) {
                j->bb = (j->bb << 8) | 0xFF;
                j->bits += 8;
                j->pos += 2;
            } else {
                j->marker = (0xFF << 8) | nb;
                j->pos += 2;
                break;
            }
        }
    }
}

static int get_bit(jpg_t* j) {
    if (j->bits == 0) {
        fill_bits(j);
        if (j->bits == 0) return 0;
    }
    j->bits--;
    return (int)((j->bb >> j->bits) & 1);
}

static int get_bits(jpg_t* j, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        v = (v << 1) | get_bit(j);
        if (j->marker) break;
    }
    return v;
}

static int extend(int v, int s) {
    if (s == 0) return 0;
    if (v < (1 << (s - 1))) return v - (1 << s) + 1;
    return v;
}

static int huff_decode(jpg_t* j, const huff_t* t) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | get_bit(j);
        if (j->marker) return 0;
        if (code <= t->maxcode[len]) {
            int idx = t->valptr[len] + (code - t->mincode[len]);
            if (idx >= 0 && idx < t->nvals) return t->huffval[idx];
            return 0;
        }
    }
    return 0;
}

static void idct_block(int* blk) {
    int tmp[64];
    for (int v = 0; v < 8; v++) {
        const int* row = blk + v * 8;
        for (int x = 0; x < 8; x++) {
            int s = 0;
            for (int u = 0; u < 8; u++) s += icos[u][x] * row[u];
            tmp[v * 8 + x] = (s + 16384) >> 15;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int s = 0;
            for (int v = 0; v < 8; v++) s += icos[v][y] * tmp[v * 8 + x];
            blk[y * 8 + x] = (s + 16384) >> 15;
        }
    }
}

static int clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static void decode_block(jpg_t* j, comp_t* c, int* dcp, int block_x, int block_y) {
    int coef[64];
    for (int i = 0; i < 64; i++) coef[i] = 0;

    int s = huff_decode(j, &j->dc_tab[c->td]);
    int diff = 0;
    if (s > 0) diff = extend(get_bits(j, s), s);
    *dcp += diff;
    coef[0] = *dcp;

    int k = 1;
    while (k < 64) {
        int rs = huff_decode(j, &j->ac_tab[c->ta]);
        if (j->marker) break;
        int r = rs >> 4;
        int s2 = rs & 0x0F;
        if (s2 == 0) {
            if (r == 15) { k += 16; continue; }
            break;
        }
        k += r;
        if (k >= 64) break;
        coef[k] = extend(get_bits(j, s2), s2);
        k++;
    }

    int blk[64];
    for (int i = 0; i < 64; i++) blk[i] = 0;
    for (int kk = 0; kk < 64; kk++) {
        int nat = zigzag[kk];
        blk[nat] = coef[kk] * (int)j->qtab[c->tq][kk];
    }
    idct_block(blk);

    int bx8 = block_x * 8;
    int by8 = block_y * 8;
    for (int y = 0; y < 8; y++) {
        int py = by8 + y;
        if (py >= c->ch) continue;
        for (int x = 0; x < 8; x++) {
            int px = bx8 + x;
            if (px >= c->cw) continue;
            c->out[py * c->cw + px] = (int16_t)(blk[y * 8 + x] + 128);
        }
    }
}

static void decode_scan(jpg_t* j) {
    int dcp[MAX_COMPS];
    for (int i = 0; i < MAX_COMPS; i++) dcp[i] = 0;

    int mcus_x, mcus_y;
    int nonint = (j->scan_ns == 1);

    if (nonint) {
        comp_t* c = &j->comp[j->scan_comp[0]];
        int cws = (j->width * c->h + j->hmax - 1) / j->hmax;
        int chs = (j->height * c->v + j->vmax - 1) / j->vmax;
        mcus_x = (cws + 7) / 8;
        mcus_y = (chs + 7) / 8;
    } else {
        int sh = 1, sv = 1;
        for (int i = 0; i < j->scan_ns; i++) {
            comp_t* c = &j->comp[j->scan_comp[i]];
            if (c->h > sh) sh = c->h;
            if (c->v > sv) sv = c->v;
        }
        mcus_x = (j->width + 8 * sh - 1) / (8 * sh);
        mcus_y = (j->height + 8 * sv - 1) / (8 * sv);
    }

    int rst_cnt = 0;
    for (int my = 0; my < mcus_y && !j->eoi; my++) {
        for (int mx = 0; mx < mcus_x && !j->eoi; mx++) {
            for (int si = 0; si < j->scan_ns; si++) {
                comp_t* c = &j->comp[j->scan_comp[si]];
                if (nonint) {
                    decode_block(j, c, &dcp[j->scan_comp[si]], mx, my);
                } else {
                    for (int by = 0; by < c->v; by++) {
                        for (int bx = 0; bx < c->h; bx++) {
                            decode_block(j, c, &dcp[j->scan_comp[si]],
                                         mx * c->h + bx, my * c->v + by);
                        }
                    }
                }
                if (j->marker) break;
            }
            if (j->marker) break;

            if (j->restart_interval > 0) {
                rst_cnt++;
                if (rst_cnt >= j->restart_interval &&
                    !(my == mcus_y - 1 && mx == mcus_x - 1)) {
                    j->bits = 0;
                    if (j->marker) {
                        int mk = j->marker & 0xFF;
                        j->marker = 0;
                        if (mk == 0xD9) { j->eoi = 1; break; }
                    } else {
                        while (j->pos < j->size && j->data[j->pos] != 0xFF) j->pos++;
                        if (j->pos + 1 < j->size) {
                            uint8_t nb = j->data[j->pos + 1];
                            if (nb >= 0xD0 && nb <= 0xD7) j->pos += 2;
                            else if (nb == 0xD9) { j->pos += 2; j->eoi = 1; break; }
                        }
                    }
                    for (int i = 0; i < MAX_COMPS; i++) dcp[i] = 0;
                    rst_cnt = 0;
                }
            }
        }
    }
}

static int parse_dqt(jpg_t* j) {
    int len = u16(j->data + j->pos);
    j->pos += 2;
    len -= 2;
    while (len > 0) {
        if (j->pos + 1 > j->size) return -1;
        int pqtq = j->data[j->pos++];
        int pq = pqtq >> 4;
        int tq = pqtq & 0xF;
        if (tq >= MAX_COMPS) return -1;
        if (pq == 0) {
            if (j->pos + 64 > j->size) return -1;
            for (int i = 0; i < 64; i++) j->qtab[tq][i] = j->data[j->pos++];
            len -= 65;
        } else {
            if (j->pos + 128 > j->size) return -1;
            for (int i = 0; i < 64; i++) {
                j->qtab[tq][i] = (uint16_t)u16(j->data + j->pos);
                j->pos += 2;
            }
            len -= 129;
        }
    }
    return 0;
}

static int parse_dht(jpg_t* j) {
    int len = u16(j->data + j->pos);
    j->pos += 2;
    len -= 2;
    while (len > 0) {
        if (j->pos + 17 > j->size) return -1;
        int tcth = j->data[j->pos++];
        int tc = tcth >> 4;
        int th = tcth & 0xF;
        if (th >= MAX_COMPS) return -1;
        uint8_t bits[16];
        int cnt = 0;
        for (int i = 0; i < 16; i++) { bits[i] = j->data[j->pos++]; cnt += bits[i]; }
        if (j->pos + cnt > j->size) return -1;
        uint8_t vals[256];
        for (int i = 0; i < cnt; i++) vals[i] = j->data[j->pos++];
        build_huff(tc ? &j->ac_tab[th] : &j->dc_tab[th], bits, vals);
        len -= 17 + cnt;
    }
    return 0;
}

static int parse_sof0(jpg_t* j) {
    int len = u16(j->data + j->pos);
    (void)len;
    j->pos += 2;
    if (j->pos + 6 > j->size) return -1;
    int prec = j->data[j->pos++];
    if (prec != 8) { serial_write("jpeg: 8-bit only\n"); return -1; }
    j->height = u16(j->data + j->pos); j->pos += 2;
    j->width = u16(j->data + j->pos); j->pos += 2;
    j->nf = j->data[j->pos++];
    if (j->nf < 1 || j->nf > MAX_COMPS) { serial_write("jpeg: bad nf\n"); return -1; }
    j->hmax = 1; j->vmax = 1;
    for (int i = 0; i < j->nf; i++) {
        if (j->pos + 3 > j->size) return -1;
        comp_t* c = &j->comp[i];
        c->id = j->data[j->pos++];
        int sf = j->data[j->pos++];
        c->h = sf >> 4;
        c->v = sf & 0xF;
        c->tq = j->data[j->pos++];
        if (c->h == 0 || c->v == 0 || c->h > 4 || c->v > 4) return -1;
        if (c->h > j->hmax) j->hmax = c->h;
        if (c->v > j->vmax) j->vmax = c->v;
    }
    return 0;
}

static int parse_dri(jpg_t* j) {
    int len = u16(j->data + j->pos);
    j->pos += 2;
    j->restart_interval = u16(j->data + j->pos);
    j->pos += 2;
    (void)len;
    return 0;
}

static int parse_sos(jpg_t* j) {
    int len = u16(j->data + j->pos);
    j->pos += 2;
    if (j->pos + 1 > j->size) return -1;
    j->scan_ns = j->data[j->pos++];
    if (j->scan_ns < 1 || j->scan_ns > MAX_COMPS) return -1;
    for (int i = 0; i < j->scan_ns; i++) {
        if (j->pos + 2 > j->size) return -1;
        int id = j->data[j->pos++];
        int tdta = j->data[j->pos++];
        int found = -1;
        for (int k = 0; k < j->nf; k++) {
            if (j->comp[k].id == id) { found = k; break; }
        }
        if (found < 0) return -1;
        j->scan_comp[i] = found;
        j->comp[found].td = tdta >> 4;
        j->comp[found].ta = tdta & 0xF;
    }
    if (j->pos + 3 > j->size) return -1;
    j->pos += 3;
    (void)len;
    return 0;
}

static int alloc_components(jpg_t* j) {
    j->fmcus_x = (j->width + 8 * j->hmax - 1) / (8 * j->hmax);
    j->fmcus_y = (j->height + 8 * j->vmax - 1) / (8 * j->vmax);
    for (int i = 0; i < j->nf; i++) {
        comp_t* c = &j->comp[i];
        c->cw = j->fmcus_x * c->h * 8;
        c->ch = j->fmcus_y * c->v * 8;
        c->out = (int16_t*)kmalloc((size_t)c->cw * c->ch * 2);
        if (!c->out) { serial_write("jpeg: oom\n"); return -1; }
        for (int k = 0; k < c->cw * c->ch; k++) c->out[k] = 0;
    }
    return 0;
}

static int build_output(jpg_t* j, uint32_t** out_pixels, int* out_w, int* out_h) {
    int W = j->width;
    int H = j->height;
    uint32_t* out = (uint32_t*)kmalloc((size_t)W * H * 4);
    if (!out) { serial_write("jpeg: oom out\n"); return -1; }

    if (j->nf == 1) {
        comp_t* y = &j->comp[0];
        for (int py = 0; py < H; py++) {
            int cy = py * y->ch / H; if (cy >= y->ch) cy = y->ch - 1;
            for (int px = 0; px < W; px++) {
                int cx = px * y->cw / W; if (cx >= y->cw) cx = y->cw - 1;
                int v = y->out[cy * y->cw + cx];
                uint32_t rgb = ((uint32_t)v << 16) | ((uint32_t)v << 8) | (uint32_t)v;
                out[py * W + px] = (0xFFu << 24) | rgb;
            }
        }
    } else {
        comp_t* y = &j->comp[0];
        comp_t* cb = &j->comp[1];
        comp_t* cr = &j->comp[2];
        for (int py = 0; py < H; py++) {
            int cy = py * y->ch / H; if (cy >= y->ch) cy = y->ch - 1;
            int cbcy = py * cb->ch / H; if (cbcy >= cb->ch) cbcy = cb->ch - 1;
            int crcy = py * cr->ch / H; if (crcy >= cr->ch) crcy = cr->ch - 1;
            for (int px = 0; px < W; px++) {
                int cyx = px * y->cw / W; if (cyx >= y->cw) cyx = y->cw - 1;
                int cbx = px * cb->cw / W; if (cbx >= cb->cw) cbx = cb->cw - 1;
                int crx = px * cr->cw / W; if (crx >= cr->cw) crx = cr->cw - 1;
                int Y = y->out[cy * y->cw + cyx];
                int Cb = cb->out[cbcy * cb->cw + cbx] - 128;
                int Cr = cr->out[crcy * cr->cw + crx] - 128;
                int r = clamp8(Y + ((Cr * 91881) >> 16));
                int g = clamp8(Y - ((Cb * 22554) >> 16) - ((Cr * 46808) >> 16));
                int b = clamp8(Y + ((Cb * 116129) >> 16));
                out[py * W + px] = (0xFFu << 24) | ((uint32_t)r << 16) |
                                  ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
    *out_pixels = out;
    *out_w = W;
    *out_h = H;
    return 0;
}

int jpeg_decode(const char* path, uint32_t** out_pixels, int* out_w, int* out_h) {
    if (!path || !out_pixels || !out_w || !out_h) return -1;
    *out_pixels = 0;
    *out_w = 0;
    *out_h = 0;

    uint32_t fsize = 0;
    uint8_t* data = read_file_all(path, &fsize);
    if (!data) {
        serial_write("jpeg: cannot read ");
        serial_write(path);
        serial_write("\n");
        return -1;
    }

    jpg_t j;
    int jsize = (int)sizeof(j);
    for (int i = 0; i < jsize; i++) ((uint8_t*)&j)[i] = 0;
    j.data = data;
    j.size = fsize;
    j.pos = 0;
    j.bits = 0;
    j.marker = 0;
    j.restart_interval = 0;
    j.scan_ns = 0;

    if (fsize < 2 || data[0] != 0xFF || data[1] != 0xD8) {
        serial_write("jpeg: bad SOI\n");
        kfree(data);
        return -1;
    }
    j.pos = 2;

    int saw_sof = 0;
    int got_scan = 0;
    while (j.pos + 1 < j.size) {
        if (j.data[j.pos] != 0xFF) { j.pos++; continue; }
        uint8_t m = j.data[j.pos + 1];
        j.pos += 2;
        if (m == 0xD9) break;
        if (m == 0x00) continue;
        if (m >= 0xD0 && m <= 0xD7) continue;
        if (m == 0xD8) continue;
        if (m == 0xC0) {
            if (parse_sof0(&j) < 0) { kfree(data); return -1; }
            saw_sof = 1;
            continue;
        }
        if (m >= 0xC1 && m <= 0xCF && m != 0xC8) {
            serial_write("jpeg: unsupported SOF\n");
            kfree(data);
            return -1;
        }
        if (m == 0xC4) { if (parse_dht(&j) < 0) { kfree(data); return -1; } continue; }
        if (m == 0xDB) { if (parse_dqt(&j) < 0) { kfree(data); return -1; } continue; }
        if (m == 0xDD) { if (parse_dri(&j) < 0) { kfree(data); return -1; } continue; }
        if (m == 0xDA) {
            if (!saw_sof) { kfree(data); return -1; }
            if (got_scan == 0) {
                if (alloc_components(&j) < 0) {
                    for (int i = 0; i < j.nf; i++) if (j.comp[i].out) kfree(j.comp[i].out);
                    kfree(data);
                    return -1;
                }
            }
            got_scan = 1;
            if (parse_sos(&j) < 0) {
                for (int i = 0; i < j.nf; i++) if (j.comp[i].out) kfree(j.comp[i].out);
                kfree(data);
                return -1;
            }
            decode_scan(&j);
            while (j.marker == 0xFFDA) {
                j.marker = 0;
                if (parse_sos(&j) < 0) break;
                decode_scan(&j);
            }
            break;
        }
        if (j.pos + 2 > j.size) { kfree(data); return -1; }
        int len = u16(j.data + j.pos);
        if (len < 2) { kfree(data); return -1; }
        j.pos += len;
    }

    if (!got_scan || !saw_sof) {
        serial_write("jpeg: incomplete\n");
        for (int i = 0; i < j.nf; i++) if (j.comp[i].out) kfree(j.comp[i].out);
        kfree(data);
        return -1;
    }

    int ret = build_output(&j, out_pixels, out_w, out_h);

    for (int i = 0; i < j.nf; i++) if (j.comp[i].out) kfree(j.comp[i].out);
    kfree(data);
    return ret;
}
