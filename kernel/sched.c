#include "sched.h"
#include "process.h"
#include <stddef.h>

volatile int sched_enabled = 0;

void sched_init(void) {
    sched_enabled = 0;
    process_t* cur = process_get_current();
    if (cur) cur->time_slice = DEFAULT_TIME_SLICE;
}

void schedule(void) {
    if (!sched_enabled) return;

    process_t* cur = process_get_current();
    if (!cur) return;

    // TERMINATED 进程主动让出 CPU
    if (cur->state == PROCESS_TERMINATED) {
        process_t* next = process_get_next_ready_after(cur);
        if (!next) {
            process_t* idle = process_get_by_pid(0);
            if (idle && idle != cur) {
                idle->state = PROCESS_RUNNING;
                process_set_current(idle);
                switch_to(cur, idle);
            }
            return;
        }
        next->state = PROCESS_RUNNING;
        process_set_current(next);
        switch_to(cur, next);
        return;
    }

    cur->time_slice--;
    if (cur->time_slice > 0) return;

    cur->time_slice = DEFAULT_TIME_SLICE;

    process_t* next = process_get_next_ready_after(cur);
    if (!next || next == cur) return;

    cur->state = PROCESS_READY;
    next->state = PROCESS_RUNNING;
    process_set_current(next);

    switch_to(cur, next);
}

void sched_enable(void) { sched_enabled = 1; }
void sched_disable(void) { sched_enabled = 0; }
