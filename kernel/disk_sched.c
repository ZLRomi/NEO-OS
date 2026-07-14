#include "disk_sched.h"
#include "disk.h"
#include "serial.h"
#include <stdint.h>
#include <stddef.h>

#define DISK_SCHED_QUEUE_SIZE 64

static disk_request_t sched_queue[DISK_SCHED_QUEUE_SIZE];
static int sched_count = 0;
static uint32_t sched_head = 0;
static int sched_direction = 1;
static disk_sched_policy_t sched_policy = DISK_SCHED_FCFS;

void disk_sched_init(void) {
    sched_count = 0;
    sched_head = 0;
    sched_direction = 1;
    sched_policy = DISK_SCHED_FCFS;
}

void disk_sched_set_policy(disk_sched_policy_t policy) {
    if ((int)policy < 0 || (int)policy > (int)DISK_SCHED_CSCAN) return;
    sched_policy = policy;
}

disk_sched_policy_t disk_sched_get_policy(void) {
    return sched_policy;
}

int disk_sched_submit(disk_request_t* req) {
    if (!req || !req->buf) return -1;
    if (sched_count >= DISK_SCHED_QUEUE_SIZE) return -1;
    sched_queue[sched_count] = *req;
    sched_queue[sched_count].done = 0;
    sched_count++;
    return 0;
}

int disk_sched_pending(void) {
    return sched_count;
}

static uint32_t abs_diff(uint32_t a, uint32_t b) {
    return (a >= b) ? (a - b) : (b - a);
}

static int select_sstf(void) {
    int best = 0;
    uint32_t best_dist = abs_diff(sched_queue[0].lba, sched_head);
    for (int i = 1; i < sched_count; i++) {
        uint32_t d = abs_diff(sched_queue[i].lba, sched_head);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

static int select_scan(void) {
    int i;
    int best = -1;
    uint32_t best_lba = 0;

    if (sched_direction == 1) {
        for (i = 0; i < sched_count; i++) {
            uint32_t l = sched_queue[i].lba;
            if (l >= sched_head && (best == -1 || l < best_lba)) {
                best_lba = l;
                best = i;
            }
        }
        if (best == -1) {
            sched_direction = -1;
            for (i = 0; i < sched_count; i++) {
                uint32_t l = sched_queue[i].lba;
                if (l <= sched_head && (best == -1 || l > best_lba)) {
                    best_lba = l;
                    best = i;
                }
            }
        }
    } else {
        for (i = 0; i < sched_count; i++) {
            uint32_t l = sched_queue[i].lba;
            if (l <= sched_head && (best == -1 || l > best_lba)) {
                best_lba = l;
                best = i;
            }
        }
        if (best == -1) {
            sched_direction = 1;
            for (i = 0; i < sched_count; i++) {
                uint32_t l = sched_queue[i].lba;
                if (l >= sched_head && (best == -1 || l < best_lba)) {
                    best_lba = l;
                    best = i;
                }
            }
        }
    }

    if (best == -1) best = 0;
    return best;
}

static int select_cscan(void) {
    int i;
    int best = -1;
    uint32_t best_lba = 0;

    for (i = 0; i < sched_count; i++) {
        uint32_t l = sched_queue[i].lba;
        if (l >= sched_head && (best == -1 || l < best_lba)) {
            best_lba = l;
            best = i;
        }
    }
    if (best != -1) return best;

    best = 0;
    best_lba = sched_queue[0].lba;
    for (i = 1; i < sched_count; i++) {
        if (sched_queue[i].lba < best_lba) {
            best_lba = sched_queue[i].lba;
            best = i;
        }
    }
    return best;
}

int disk_sched_dispatch(void) {
    if (sched_count <= 0) return 0;

    int idx;
    switch (sched_policy) {
        case DISK_SCHED_SSTF:  idx = select_sstf();  break;
        case DISK_SCHED_SCAN:  idx = select_scan();  break;
        case DISK_SCHED_CSCAN: idx = select_cscan(); break;
        default:               idx = 0;              break;
    }

    disk_request_t* r = &sched_queue[idx];
    int rc;
    if (r->is_write) {
        rc = disk_write_sector(r->lba, (const uint8_t*)r->buf);
    } else {
        rc = disk_read_sector(r->lba, (uint8_t*)r->buf);
    }

    r->done = 1;
    sched_head = r->lba;

    for (int i = idx; i < sched_count - 1; i++) {
        sched_queue[i] = sched_queue[i + 1];
    }
    sched_count--;

    if (rc < 0) {
        serial_write("disk_sched: I/O error at lba\n");
        return -1;
    }
    return 1;
}
