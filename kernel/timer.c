#include "timer.h"
#include "idt.h"
#include "sched.h"
#include "klog.h"
#include <stdint.h>

#define MAX_TIMERS 32

static kernel_timer_t timers[MAX_TIMERS];

void timer_init(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        timers[i].active = 0;
        timers[i].expire_tick = 0;
        timers[i].callback = 0;
        timers[i].arg = 0;
    }
    klog_write_str("timer: service initialized (100Hz)\n");
}

int timer_add(uint32_t ticks, timer_callback_t cb, void* arg) {
    if (!cb) return -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].expire_tick = timer_ticks + ticks;
            timers[i].callback = cb;
            timers[i].arg = arg;
            timers[i].active = 1;
            return i;
        }
    }
    return -1;
}

void timer_tick(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) continue;
        if ((int32_t)(timer_ticks - timers[i].expire_tick) >= 0) {
            timers[i].active = 0;
            timer_callback_t cb = timers[i].callback;
            void* arg = timers[i].arg;
            cb(arg);
        }
    }
}

uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

void timer_sleep(uint32_t ticks) {
    uint32_t start = timer_ticks;
    while (timer_ticks - start < ticks) {
        schedule();
    }
}
