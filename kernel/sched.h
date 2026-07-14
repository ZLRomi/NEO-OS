#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>

#define DEFAULT_TIME_SLICE 10

void sched_init(void);
void schedule(void);
void sched_enable(void);
void sched_disable(void);

extern volatile int sched_enabled;

#endif
