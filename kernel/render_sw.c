#include "render_sw.h"
#include "gui_internal.h"
#include "gfx_3d.h"
#include <stdint.h>

typedef struct {
    fixed_t x, y, z;
    fixed_t r, g, b;
} svert;

static int clamp255(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

void render_clear(void) {
    if (back_buffer) {
        uint32_t words = (fb_height * fb_pitch) / 4;
        for (uint32_t i = 0; i < words; i++) back_buffer[i] = 0;
    }
    zbuffer_clear(zbuffer_get_width(), zbuffer_get_height());
}

static void edge_interp(const svert* a, const svert* b, fixed_t y, svert* out) {
    fixed_t dy = b->y - a->y;
    if (dy == 0) { *out = *a; return; }
    fixed_t t = fixed_div(y - a->y, dy);
    if (t < 0) t = 0;
    else if (t > FIXED_ONE) t = FIXED_ONE;
    out->x = a->x + fixed_mul(b->x - a->x, t);
    out->z = a->z + fixed_mul(b->z - a->z, t);
    out->r = a->r + fixed_mul(b->r - a->r, t);
    out->g = a->g + fixed_mul(b->g - a->g, t);
    out->b = a->b + fixed_mul(b->b - a->b, t);
    out->y = y;
}

void render_triangle(vertex3d v0, vertex3d v1, vertex3d v2) {
    if (v0.w <= 0 || v1.w <= 0 || v2.w <= 0) return;

    const vertex3d* vin[3] = { &v0, &v1, &v2 };
    svert s[3];
    for (int i = 0; i < 3; i++) {
        fixed_t invw = fixed_div(FIXED_ONE, vin[i]->w);
        fixed_t ndcx = fixed_mul(vin[i]->pos.x, invw);
        fixed_t ndcy = fixed_mul(vin[i]->pos.y, invw);
        fixed_t ndcz = fixed_mul(vin[i]->pos.z, invw);
        s[i].x = fixed_mul(ndcx + FIXED_ONE, INT_TO_FIXED((int)fb_width) >> 1);
        s[i].y = fixed_mul(FIXED_ONE - ndcy, INT_TO_FIXED((int)fb_height) >> 1);
        s[i].z = fixed_mul(ndcz + FIXED_ONE, FIXED_ONE >> 1);
        s[i].r = INT_TO_FIXED((int)((vin[i]->color >> 16) & 0xFF));
        s[i].g = INT_TO_FIXED((int)((vin[i]->color >> 8) & 0xFF));
        s[i].b = INT_TO_FIXED((int)(vin[i]->color & 0xFF));
    }

    if (s[1].y < s[0].y) { svert t = s[0]; s[0] = s[1]; s[1] = t; }
    if (s[2].y < s[0].y) { svert t = s[0]; s[0] = s[2]; s[2] = t; }
    if (s[2].y < s[1].y) { svert t = s[1]; s[1] = s[2]; s[2] = t; }

    if (s[2].y - s[0].y == 0) return;

    int W = (int)fb_width;
    int H = (int)fb_height;
    int pitch_w = (int)(fb_pitch / 4);
    fixed_t* zb = zbuffer_get();

    int y0i = FIXED_TO_INT(s[0].y);
    int y2i = FIXED_TO_INT(s[2].y);
    if (y0i < 0) y0i = 0;
    if (y2i >= H) y2i = H - 1;

    for (int y = y0i; y <= y2i; y++) {
        fixed_t fy = INT_TO_FIXED(y) + (FIXED_ONE >> 1);

        svert L, R;
        edge_interp(&s[0], &s[2], fy, &L);
        if (fy < s[1].y) {
            edge_interp(&s[0], &s[1], fy, &R);
        } else {
            edge_interp(&s[1], &s[2], fy, &R);
        }

        if (L.x > R.x) { svert t = L; L = R; R = t; }

        int x0i = FIXED_TO_INT(L.x);
        int x1i = FIXED_TO_INT(R.x);
        if (x0i < 0) x0i = 0;
        if (x1i >= W) x1i = W - 1;
        if (x0i > x1i) continue;

        fixed_t dx = R.x - L.x;
        fixed_t inv_dx = (dx != 0) ? fixed_div(FIXED_ONE, dx) : FIXED_ONE;
        fixed_t dz = fixed_mul(R.z - L.z, inv_dx);
        fixed_t dr = fixed_mul(R.r - L.r, inv_dx);
        fixed_t dg = fixed_mul(R.g - L.g, inv_dx);
        fixed_t db = fixed_mul(R.b - L.b, inv_dx);

        fixed_t fz = L.z + fixed_mul(dz, INT_TO_FIXED(x0i) + (FIXED_ONE >> 1) - L.x);
        fixed_t fr = L.r + fixed_mul(dr, INT_TO_FIXED(x0i) + (FIXED_ONE >> 1) - L.x);
        fixed_t fg = L.g + fixed_mul(dg, INT_TO_FIXED(x0i) + (FIXED_ONE >> 1) - L.x);
        fixed_t fb = L.b + fixed_mul(db, INT_TO_FIXED(x0i) + (FIXED_ONE >> 1) - L.x);

        for (int x = x0i; x <= x1i; x++) {
            fixed_t z = fz;
            int draw = 1;
            if (zb) {
                int idx = y * W + x;
                if (zb[idx] > z) {
                    zb[idx] = z;
                } else {
                    draw = 0;
                }
            }
            if (draw) {
                uint32_t r = (uint32_t)clamp255(FIXED_TO_INT(fr));
                uint32_t g = (uint32_t)clamp255(FIXED_TO_INT(fg));
                uint32_t b = (uint32_t)clamp255(FIXED_TO_INT(fb));
                back_buffer[y * pitch_w + x] = (r << 16) | (g << 8) | b;
            }
            fz += dz;
            fr += dr;
            fg += dg;
            fb += db;
        }
    }
}
