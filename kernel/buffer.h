#ifndef BUFFER_H
#define BUFFER_H
#include <stdint.h>

#define BUF_HASH_SIZE 64
#define MAX_BUFFERS 64

typedef struct buffer {
    uint32_t dev;
    uint32_t block;
    uint8_t  data[512];
    int      valid;
    int      dirty;
    struct buffer* next;
    struct buffer* lru_prev;
    struct buffer* lru_next;
} buffer_t;

buffer_t* buffer_read(uint32_t block);
void buffer_mark_dirty(buffer_t* buf);
void buffer_release(buffer_t* buf);
void buffer_sync_all(void);
void buffer_init(void);

#endif
