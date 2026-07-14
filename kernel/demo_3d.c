#include "demo_3d.h"
#include "render_sw.h"
#include "gfx_3d.h"
#include "gui_internal.h"
#include "idt.h"
#include <stdint.h>

extern volatile int gui_dirty;

static int demo_active = 0;

#define ANGLE_PER_TICK 2059

static const vec3f cube_verts[8] = {
    { -FIXED_ONE, -FIXED_ONE, -FIXED_ONE },
    {  FIXED_ONE, -FIXED_ONE, -FIXED_ONE },
    {  FIXED_ONE,  FIXED_ONE, -FIXED_ONE },
    { -FIXED_ONE,  FIXED_ONE, -FIXED_ONE },
    { -FIXED_ONE, -FIXED_ONE,  FIXED_ONE },
    {  FIXED_ONE, -FIXED_ONE,  FIXED_ONE },
    {  FIXED_ONE,  FIXED_ONE,  FIXED_ONE },
    { -FIXED_ONE,  FIXED_ONE,  FIXED_ONE }
};

static const int tri_indices[12][3] = {
    { 4, 5, 6 }, { 4, 6, 7 },
    { 1, 0, 3 }, { 1, 3, 2 },
    { 0, 3, 7 }, { 0, 7, 4 },
    { 5, 1, 2 }, { 5, 2, 6 },
    { 0, 4, 5 }, { 0, 5, 1 },
    { 3, 2, 6 }, { 3, 6, 7 }
};

static const uint32_t vert_colors[8] = {
    0x00FF4040, 0x0040FF40, 0x004040FF, 0x00FFFF40,
    0x0040FFFF, 0x00FF40FF, 0x00FFFFFF, 0x00FFA040
};

void demo_3d_render(void) {
    int W = (int)fb_width;
    int H = (int)fb_height;
    if (W == 0 || H == 0) return;

    fixed_t aspect = fixed_div(INT_TO_FIXED(W), INT_TO_FIXED(H));
    fixed_t near = fixed_div(FIXED_ONE, INT_TO_FIXED(10));
    fixed_t far = INT_TO_FIXED(100);
    fixed_t fov = 60 * 1143;
    fixed_t angle = (fixed_t)((int)timer_ticks * ANGLE_PER_TICK);

    mat4f proj, view, rotY, rotX, model, vm, mvp;
    mat4_perspective(&proj, fov, aspect, near, far);
    mat4_translate(&view, 0, 0, -INT_TO_FIXED(3));
    mat4_rotate_y(&rotY, angle);
    mat4_rotate_x(&rotX, angle >> 1);
    mat4_multiply(&model, &rotY, &rotX);
    mat4_multiply(&vm, &view, &model);
    mat4_multiply(&mvp, &proj, &vm);

    render_clear();

    for (int t = 0; t < 12; t++) {
        int i0 = tri_indices[t][0];
        int i1 = tri_indices[t][1];
        int i2 = tri_indices[t][2];
        vec4f c0 = mat4_transform_vec4(&mvp, cube_verts[i0]);
        vec4f c1 = mat4_transform_vec4(&mvp, cube_verts[i1]);
        vec4f c2 = mat4_transform_vec4(&mvp, cube_verts[i2]);
        vertex3d v0 = { { c0.x, c0.y, c0.z }, c0.w, vert_colors[i0] };
        vertex3d v1 = { { c1.x, c1.y, c1.z }, c1.w, vert_colors[i1] };
        vertex3d v2 = { { c2.x, c2.y, c2.z }, c2.w, vert_colors[i2] };
        render_triangle(v0, v1, v2);
    }

    fb_blit();
}

void demo_3d_toggle(void) {
    demo_active = !demo_active;
    gui_dirty = 1;
}

int demo_3d_is_active(void) {
    return demo_active;
}

void demo_3d_run(void) {
    demo_3d_toggle();
}
