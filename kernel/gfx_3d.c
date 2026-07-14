#include "gfx_3d.h"
#include "gfx_driver.h"
#include "memory.h"
#include "printk.h"
#include <stdint.h>

static fixed_t sin_table[256];
static int sin_inited = 0;

static fixed_t* zbuffer = 0;
static int zb_w = 0;
static int zb_h = 0;

typedef unsigned long long u64_t;
typedef long long s64_t;

__attribute__((noinline, optimize("-fno-tree-loop-distribute-patterns")))
static u64_t udiv64(u64_t n, u64_t d) {
    if (d == 0) return 0;
    u64_t q = 0;
    u64_t r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1ULL);
        if (r >= d) { r -= d; q |= (1ULL << i); }
    }
    return q;
}

__attribute__((noinline, used, optimize("-fno-tree-loop-distribute-patterns")))
s64_t __divdi3(s64_t a, s64_t b) {
    int neg = 0;
    u64_t ua, ub;
    if (a < 0) { ua = (u64_t)(-a); neg = !neg; } else ua = (u64_t)a;
    if (b < 0) { ub = (u64_t)(-b); neg = !neg; } else ub = (u64_t)b;
    u64_t q = udiv64(ua, ub);
    return neg ? -(s64_t)q : (s64_t)q;
}

fixed_t fixed_mul(fixed_t a, fixed_t b) {
    return (fixed_t)(((int64_t)a * b) >> FIXED_SHIFT);
}

fixed_t fixed_div(fixed_t a, fixed_t b) {
    if (b == 0) return 0;
    return (fixed_t)(((int64_t)a << FIXED_SHIFT) / b);
}

static fixed_t sin_taylor(fixed_t x) {
    int64_t x2 = ((int64_t)x * x) >> FIXED_SHIFT;
    int64_t x3 = (x2 * x) >> FIXED_SHIFT;
    int64_t x5 = (x3 * x2) >> FIXED_SHIFT;
    int64_t x7 = (x5 * x2) >> FIXED_SHIFT;
    fixed_t t1 = x;
    fixed_t t3 = (fixed_t)(x3 / 6);
    fixed_t t5 = (fixed_t)(x5 / 120);
    fixed_t t7 = (fixed_t)(x7 / 5040);
    return t1 - t3 + t5 - t7;
}

static fixed_t compute_sin(fixed_t angle) {
    fixed_t a = angle;
    while (a < 0) a += FIXED_TWO_PI;
    while (a >= FIXED_TWO_PI) a -= FIXED_TWO_PI;
    int sign = 1;
    if (a >= FIXED_PI) { a = a - FIXED_PI; sign = -1; }
    if (a > FIXED_HALF_PI) a = FIXED_PI - a;
    return (sign > 0) ? sin_taylor(a) : -sin_taylor(a);
}

static void init_sin_table(void) {
    for (int i = 0; i < 256; i++) {
        fixed_t angle = (fixed_t)(((int64_t)i * FIXED_TWO_PI) / 256);
        sin_table[i] = compute_sin(angle);
    }
    sin_inited = 1;
}

fixed_t fixed_sin(fixed_t x) {
    if (!sin_inited) init_sin_table();
    int64_t idx64 = ((int64_t)x * 256) / FIXED_TWO_PI;
    int idx = ((int)idx64) & 255;
    return sin_table[idx];
}

fixed_t fixed_cos(fixed_t x) {
    return fixed_sin(x + FIXED_HALF_PI);
}

void mat4_identity(mat4f* m) {
    for (int i = 0; i < 16; i++) m->m[i] = 0;
    m->m[0] = FIXED_ONE;
    m->m[5] = FIXED_ONE;
    m->m[10] = FIXED_ONE;
    m->m[15] = FIXED_ONE;
}

void mat4_multiply(mat4f* out, const mat4f* a, const mat4f* b) {
    mat4f r;
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            fixed_t sum = 0;
            for (int k = 0; k < 4; k++) {
                sum += fixed_mul(a->m[k * 4 + row], b->m[col * 4 + k]);
            }
            r.m[col * 4 + row] = sum;
        }
    }
    *out = r;
}

void mat4_perspective(mat4f* m, fixed_t fov, fixed_t aspect, fixed_t near, fixed_t far) {
    for (int i = 0; i < 16; i++) m->m[i] = 0;
    fixed_t half = fov >> 1;
    fixed_t f = fixed_div(fixed_cos(half), fixed_sin(half));
    m->m[0] = fixed_div(f, aspect);
    m->m[5] = f;
    m->m[10] = fixed_div(far + near, near - far);
    m->m[11] = -FIXED_ONE;
    fixed_t nf = fixed_mul(near, far);
    m->m[14] = fixed_div(fixed_mul(nf, INT_TO_FIXED(2)), near - far);
    m->m[15] = 0;
}

void mat4_translate(mat4f* m, fixed_t x, fixed_t y, fixed_t z) {
    mat4_identity(m);
    m->m[12] = x;
    m->m[13] = y;
    m->m[14] = z;
}

void mat4_rotate_y(mat4f* m, fixed_t angle) {
    fixed_t c = fixed_cos(angle);
    fixed_t s = fixed_sin(angle);
    for (int i = 0; i < 16; i++) m->m[i] = 0;
    m->m[0] = c;
    m->m[2] = s;
    m->m[5] = FIXED_ONE;
    m->m[8] = -s;
    m->m[10] = c;
    m->m[15] = FIXED_ONE;
}

void mat4_rotate_x(mat4f* m, fixed_t angle) {
    fixed_t c = fixed_cos(angle);
    fixed_t s = fixed_sin(angle);
    for (int i = 0; i < 16; i++) m->m[i] = 0;
    m->m[0] = FIXED_ONE;
    m->m[5] = c;
    m->m[6] = s;
    m->m[9] = -s;
    m->m[10] = c;
    m->m[15] = FIXED_ONE;
}

vec3f mat4_transform_vec(const mat4f* m, vec3f v) {
    vec3f o;
    o.x = fixed_mul(m->m[0], v.x) + fixed_mul(m->m[4], v.y) + fixed_mul(m->m[8], v.z) + m->m[12];
    o.y = fixed_mul(m->m[1], v.x) + fixed_mul(m->m[5], v.y) + fixed_mul(m->m[9], v.z) + m->m[13];
    o.z = fixed_mul(m->m[2], v.x) + fixed_mul(m->m[6], v.y) + fixed_mul(m->m[10], v.z) + m->m[14];
    return o;
}

vec4f mat4_transform_vec4(const mat4f* m, vec3f v) {
    vec4f o;
    o.x = fixed_mul(m->m[0], v.x) + fixed_mul(m->m[4], v.y) + fixed_mul(m->m[8], v.z) + m->m[12];
    o.y = fixed_mul(m->m[1], v.x) + fixed_mul(m->m[5], v.y) + fixed_mul(m->m[9], v.z) + m->m[13];
    o.z = fixed_mul(m->m[2], v.x) + fixed_mul(m->m[6], v.y) + fixed_mul(m->m[10], v.z) + m->m[14];
    o.w = fixed_mul(m->m[3], v.x) + fixed_mul(m->m[7], v.y) + fixed_mul(m->m[11], v.z) + m->m[15];
    return o;
}

void zbuffer_clear(int w, int h) {
    (void)w; (void)h;
    if (!zbuffer) return;
    int n = zb_w * zb_h;
    for (int i = 0; i < n; i++) zbuffer[i] = 0x7FFFFFFF;
}

void zbuffer_free(void) {
    zbuffer = 0;
    zb_w = 0;
    zb_h = 0;
}

fixed_t* zbuffer_get(void) { return zbuffer; }
int zbuffer_get_width(void) { return zb_w; }
int zbuffer_get_height(void) { return zb_h; }

int gfx_3d_init(void) {
    if (!sin_inited) init_sin_table();

    int w = gfx_get_width();
    int h = gfx_get_height();
    zb_w = w;
    zb_h = h;

    if (zbuffer) return 1;

    uint32_t size = (uint32_t)w * (uint32_t)h * sizeof(fixed_t);
    uint32_t pages = (size + 4095) / 4096;
    static uint32_t next_virt = 0xF2000000;
    uint32_t virt = next_virt;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) {
            printk("3D: zbuffer alloc failed (need %d pages), depth test disabled\n", (int)pages);
            zbuffer = 0;
            zb_w = 0;
            zb_h = 0;
            return 0;
        }
        if (!map_page(virt + i * 4096, phys, PAGE_PRESENT | PAGE_WRITE)) {
            pmm_free_page(phys);
            printk("3D: zbuffer map failed, depth test disabled\n");
            zbuffer = 0;
            zb_w = 0;
            zb_h = 0;
            return 0;
        }
    }
    next_virt += pages * 4096;
    zbuffer = (fixed_t*)virt;
    printk("3D: zbuffer %dx%d (%d pages)\n", w, h, (int)pages);
    return 1;
}

void gfx_3d_begin_frame(void) {
    zbuffer_clear(zb_w, zb_h);
}

void gfx_3d_end_frame(void) {
}
