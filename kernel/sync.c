#include "sync.h"
#include "process.h"
#include "sched.h"

// 调度默认禁用，阻塞操作不真正挂起进程，避免卡死单任务系统
extern volatile int sched_enabled;

// ===== 自旋锁 =====

void spin_init(spinlock_t* lk) {
    lk->locked = 0;
}

void spin_lock(spinlock_t* lk) {
    while (__sync_lock_test_and_set(&lk->locked, 1)) {
        /* 自旋等待 */
    }
}

void spin_unlock(spinlock_t* lk) {
    __sync_lock_release(&lk->locked);
}

// ===== 信号量 =====

void sem_init(semaphore_t* sem, int count) {
    sem->count = count;
    sem->wait_count = 0;
}

void sem_wait(semaphore_t* sem) {
    __asm__ volatile("cli");

    if (sem->count > 0) {
        sem->count--;
        __asm__ volatile("sti");
        return;
    }

    // count <= 0，需要阻塞
    if (!sched_enabled) {
        // 调度禁用，不真正阻塞，直接返回
        __asm__ volatile("sti");
        return;
    }

    // 加入等待队列并阻塞当前进程
    if (sem->wait_count < MAX_PROCESSES) {
        process_t* cur = process_get_current();
        if (cur) {
            sem->waiting[sem->wait_count] = cur->pid;
            sem->wait_count++;
        }
    }
    process_block(PROCESS_BLOCKED);
    __asm__ volatile("sti");
}

void sem_post(semaphore_t* sem) {
    __asm__ volatile("cli");

    if (sem->wait_count > 0) {
        // 唤醒第一个等待者
        int pid = sem->waiting[0];
        int i;
        for (i = 1; i < sem->wait_count; i++) {
            sem->waiting[i - 1] = sem->waiting[i];
        }
        sem->wait_count--;
        process_wake(pid);
    } else {
        sem->count++;
    }

    __asm__ volatile("sti");
}

// ===== 互斥锁 =====

void mutex_init(mutex_t* m) {
    m->locked = 0;
    m->owner_pid = 0;
}

void mutex_lock(mutex_t* m) {
    int pid = 0;
    process_t* cur = process_get_current();
    if (cur) pid = cur->pid;

    while (__sync_lock_test_and_set(&m->locked, 1)) {
        /* 自旋等待 */
    }
    m->owner_pid = pid;
}

void mutex_unlock(mutex_t* m) {
    m->owner_pid = 0;
    __sync_lock_release(&m->locked);
}
