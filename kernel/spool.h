#ifndef SPOOL_H
#define SPOOL_H

#include <stdint.h>

#define MAX_SPOOL_JOBS 16

#define SPOOL_STATUS_PENDING   0
#define SPOOL_STATUS_PRINTING  1
#define SPOOL_STATUS_DONE      2
#define SPOOL_STATUS_CANCELLED 3

typedef struct {
    int job_id;
    char name[32];
    char owner[32];
    uint32_t size;
    int status;
} print_job_t;

void spool_init(void);
int spool_submit(const char* name, const void* data, uint32_t size);
int spool_cancel(int job_id);
int spool_list(print_job_t* out, int max_count);
int spool_count(void);
int spool_process_next(void);

#endif
