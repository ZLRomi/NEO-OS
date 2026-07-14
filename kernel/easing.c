#include "easing.h"
#include "idt.h"

static anim_t anims[ANIM_MAX];

int ease_out_cubic(int t) {
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    int d = 1000 - t;
    int d3 = d * d * d;
    return (1000000000 - d3) / 1000000;
}

int ease_in_out_cubic(int t) {
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    if (t < 500) {
        return (4 * t * t * t) / 1000000;
    }
    int u = 2000 - 2 * t;
    int u3 = u * u * u;
    return (2000000000 - u3) / 2000000;
}

int ease_in_quad(int t) {
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    return (t * t) / 1000;
}

int ease_out_back(int t) {
    if (t <= 0) return 0;
    if (t >= 1000) return 1000;
    int d = t - 1000;
    int d2 = d * d;
    int d3 = d2 * d;
    int term3 = d3 / 1000000;
    int term2 = d2 / 1000;
    int r = 1000 + 2702 * term3 / 1000 + 1702 * term2 / 1000;
    if (r < 0) r = 0;
    return r;
}

int anim_start(int *target, uint32_t duration_ms, int from, int to, anim_complete_cb callback) {
    for (int i = 0; i < ANIM_MAX; i++) {
        if (anims[i].active) continue;
        anims[i].id = i + 1;
        anims[i].active = 1;
        anims[i].start_tick = timer_ticks;
        anims[i].duration_ms = duration_ms ? duration_ms : 1;
        anims[i].from = from;
        anims[i].to = to;
        anims[i].current = from;
        anims[i].target = target;
        anims[i].on_complete = callback;
        if (target) *target = from;
        return anims[i].id;
    }
    return 0;
}

void anim_update(void) {
    for (int i = 0; i < ANIM_MAX; i++) {
        anim_t *a = &anims[i];
        if (!a->active) continue;
        uint32_t duration_ticks = (a->duration_ms + 19u) / 20u;
        uint32_t elapsed = timer_ticks - a->start_tick;
        if (elapsed >= duration_ticks) {
            a->current = a->to;
            if (a->target) *a->target = a->current;
            a->active = 0;
            if (a->on_complete) a->on_complete(a->id);
            continue;
        }
        int t = (int)(elapsed * 20000u / a->duration_ms);
        if (t > 1000) t = 1000;
        int eased = ease_out_cubic(t);
        a->current = a->from + (a->to - a->from) * eased / 1000;
        if (a->target) *a->target = a->current;
    }
}

int anim_get(int anim_id) {
    if (anim_id < 1 || anim_id > ANIM_MAX) return 0;
    return anims[anim_id - 1].current;
}

void anim_stop(int anim_id) {
    if (anim_id < 1 || anim_id > ANIM_MAX) return;
    anims[anim_id - 1].active = 0;
}
