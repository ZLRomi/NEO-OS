#include "buffer.h"
#include "disk.h"
#include "printk.h"
#include <stdint.h>

static buffer_t buffers[MAX_BUFFERS];
static buffer_t* buffer_head[BUF_HASH_SIZE];
static buffer_t* lru_head;
static buffer_t* lru_tail;

static uint32_t buf_hash(uint32_t dev, uint32_t block) {
    return (dev ^ block) % BUF_HASH_SIZE;
}

static void hash_insert(buffer_t* b) {
    uint32_t h = buf_hash(b->dev, b->block);
    b->next = buffer_head[h];
    buffer_head[h] = b;
}

static void hash_remove(buffer_t* b) {
    uint32_t h = buf_hash(b->dev, b->block);
    buffer_t** pp = &buffer_head[h];
    while (*pp) {
        if (*pp == b) {
            *pp = b->next;
            b->next = 0;
            return;
        }
        pp = &(*pp)->next;
    }
}

static buffer_t* hash_lookup(uint32_t dev, uint32_t block) {
    uint32_t h = buf_hash(dev, block);
    buffer_t* b = buffer_head[h];
    while (b) {
        if (b->dev == dev && b->block == block) return b;
        b = b->next;
    }
    return 0;
}

static void lru_unlink(buffer_t* b) {
    if (b->lru_prev) b->lru_prev->lru_next = b->lru_next;
    else lru_head = b->lru_next;
    if (b->lru_next) b->lru_next->lru_prev = b->lru_prev;
    else lru_tail = b->lru_prev;
    b->lru_prev = 0;
    b->lru_next = 0;
}

static void lru_push_front(buffer_t* b) {
    b->lru_prev = 0;
    b->lru_next = lru_head;
    if (lru_head) lru_head->lru_prev = b;
    lru_head = b;
    if (!lru_tail) lru_tail = b;
}

static buffer_t* alloc_free_buffer(void) {
    int i;
    for (i = 0; i < MAX_BUFFERS; i++) {
        if (!buffers[i].valid) return &buffers[i];
    }
    if (lru_tail) {
        buffer_t* victim = lru_tail;
        if (victim->dirty) {
            disk_write_sector(victim->block, victim->data);
        }
        hash_remove(victim);
        victim->valid = 0;
        victim->dirty = 0;
        lru_unlink(victim);
        return victim;
    }
    return 0;
}

buffer_t* buffer_read(uint32_t block) {
    buffer_t* b = hash_lookup(0, block);
    if (b) {
        lru_unlink(b);
        lru_push_front(b);
        return b;
    }

    b = alloc_free_buffer();
    if (!b) return 0;

    if (disk_read_sector(block, b->data) < 0) {
        lru_push_front(b);
        return 0;
    }

    b->dev = 0;
    b->block = block;
    b->valid = 1;
    b->dirty = 0;
    hash_insert(b);
    lru_push_front(b);
    return b;
}

void buffer_mark_dirty(buffer_t* buf) {
    if (buf) buf->dirty = 1;
}

void buffer_release(buffer_t* buf) {
    if (!buf) return;
    lru_unlink(buf);
    lru_push_front(buf);
}

void buffer_sync_all(void) {
    int i;
    int synced = 0;
    for (i = 0; i < MAX_BUFFERS; i++) {
        if (buffers[i].valid && buffers[i].dirty) {
            disk_write_sector(buffers[i].block, buffers[i].data);
            buffers[i].dirty = 0;
            synced++;
        }
    }
    if (synced) printk("buffer: synced %d dirty buffers\n", synced);
}

void buffer_init(void) {
    int i;
    for (i = 0; i < BUF_HASH_SIZE; i++) buffer_head[i] = 0;
    for (i = 0; i < MAX_BUFFERS; i++) {
        buffers[i].dev = 0;
        buffers[i].block = 0;
        buffers[i].valid = 0;
        buffers[i].dirty = 0;
        buffers[i].next = 0;
        buffers[i].lru_prev = 0;
        buffers[i].lru_next = 0;
    }
    lru_head = 0;
    lru_tail = 0;
    printk("buffer: cache initialized (%d buffers, hash=%d)\n", MAX_BUFFERS, BUF_HASH_SIZE);
}
