#ifndef DISK_SCHED_H
#define DISK_SCHED_H

#include <stdint.h>

typedef enum {
    DISK_SCHED_FCFS  = 0,
    DISK_SCHED_SSTF  = 1,
    DISK_SCHED_SCAN  = 2,
    DISK_SCHED_CSCAN = 3
} disk_sched_policy_t;

typedef struct {
    uint32_t lba;
    void*   buf;
    int     is_write;
    int     done;
} disk_request_t;

void disk_sched_init(void);
void disk_sched_set_policy(disk_sched_policy_t policy);
disk_sched_policy_t disk_sched_get_policy(void);
int  disk_sched_submit(disk_request_t* req);
int  disk_sched_dispatch(void);
int  disk_sched_pending(void);

#endif
