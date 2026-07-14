#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

typedef void (*timer_callback_t)(void* arg);

typedef struct {
    int active;
    uint32_t expire_tick;
    timer_callback_t callback;
    void* arg;
} kernel_timer_t;

void timer_init(void);
int timer_add(uint32_t ticks, timer_callback_t cb, void* arg);
void timer_tick(void);
uint32_t timer_get_ticks(void);
void timer_sleep(uint32_t ticks);

#endif
