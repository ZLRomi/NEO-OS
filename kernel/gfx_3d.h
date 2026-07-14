#ifndef GFX_3D_H
#define GFX_3D_H
#include <stdint.h>

typedef int32_t fixed_t;

#define FIXED_SHIFT 16
#define FIXED_ONE   (1 << FIXED_SHIFT)
#define INT_TO_FIXED(x) ((fixed_t)((x) << FIXED_SHIFT))
#define FLOAT_TO_FIXED(x) ((fixed_t)((x) * (1 << FIXED_SHIFT)))
#define FIXED_TO_INT(x) ((int)((x) >> FIXED_SHIFT))

#define FIXED_PI      205887
#define FIXED_TWO_PI  411775
#define FIXED_HALF_PI 102944

typedef struct { fixed_t x, y, z; } vec3f;
typedef struct { fixed_t x, y, z, w; } vec4f;
typedef struct { fixed_t m[16]; } mat4f;

fixed_t fixed_mul(fixed_t a, fixed_t b);
fixed_t fixed_div(fixed_t a, fixed_t b);

void mat4_identity(mat4f* m);
void mat4_multiply(mat4f* out, const mat4f* a, const mat4f* b);
void mat4_perspective(mat4f* m, fixed_t fov, fixed_t aspect, fixed_t near, fixed_t far);
void mat4_translate(mat4f* m, fixed_t x, fixed_t y, fixed_t z);
void mat4_rotate_y(mat4f* m, fixed_t angle);
void mat4_rotate_x(mat4f* m, fixed_t angle);
vec3f mat4_transform_vec(const mat4f* m, vec3f v);
vec4f mat4_transform_vec4(const mat4f* m, vec3f v);

fixed_t fixed_sin(fixed_t x);
fixed_t fixed_cos(fixed_t x);

void zbuffer_clear(int w, int h);
void zbuffer_free(void);
fixed_t* zbuffer_get(void);
int zbuffer_get_width(void);
int zbuffer_get_height(void);

int gfx_3d_init(void);
void gfx_3d_begin_frame(void);
void gfx_3d_end_frame(void);

#endif
