#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>
#include "process.h"

// 自旋锁
typedef struct {
    volatile int locked;
} spinlock_t;

// 信号量
typedef struct {
    volatile int count;
    volatile int waiting[MAX_PROCESSES];
    int wait_count;
} semaphore_t;

// 互斥锁
typedef struct {
    volatile int locked;
    volatile int owner_pid;
} mutex_t;

// 自旋锁接口
void spin_init(spinlock_t* lk);
void spin_lock(spinlock_t* lk);
void spin_unlock(spinlock_t* lk);

// 信号量接口
void sem_init(semaphore_t* sem, int count);
void sem_wait(semaphore_t* sem);
void sem_post(semaphore_t* sem);

// 互斥锁接口
void mutex_init(mutex_t* m);
void mutex_lock(mutex_t* m);
void mutex_unlock(mutex_t* m);

#endif
