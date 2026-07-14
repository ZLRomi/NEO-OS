#ifndef EASING_H
#define EASING_H

#include <stdint.h>

#define ANIM_MAX 16

typedef void (*anim_complete_cb)(int anim_id);

typedef struct {
    int id;
    int active;
    uint32_t start_tick;
    uint32_t duration_ms;
    int from;
    int to;
    int current;
    int *target;
    anim_complete_cb on_complete;
} anim_t;

int ease_out_cubic(int t);
int ease_in_out_cubic(int t);
int ease_in_quad(int t);
int ease_out_back(int t);

int  anim_start(int *target, uint32_t duration_ms, int from, int to, anim_complete_cb callback);
void anim_update(void);
int  anim_get(int anim_id);
void anim_stop(int anim_id);

#endif
