#include "ipc.h"
#include "heap.h"
#include "memory.h"
#include "printk.h"
#include <stddef.h>

// 共享内存虚拟地址分配起点
static uint32_t shm_next_vaddr = 0xE0000000;

// ===== 管道 =====

void pipe_init(pipe_t* p) {
    int i;
    for (i = 0; i < PIPE_BUF_SIZE; i++) p->buffer[i] = 0;
    p->read_pos = 0;
    p->write_pos = 0;
    sem_init(&p->sem_read, 0);
    // 一个槽位用于区分满/空，可用空间为 PIPE_BUF_SIZE - 1
    sem_init(&p->sem_write, PIPE_BUF_SIZE - 1);
}

int pipe_write(pipe_t* p, const char* buf, int n) {
    int i;
    for (i = 0; i < n; i++) {
        sem_wait(&p->sem_write);
        // 调度禁用时 sem_wait 不阻塞，需检查实际是否有空间
        if ((p->write_pos + 1) % PIPE_BUF_SIZE == p->read_pos) {
            break;  // 缓冲已满
        }
        p->buffer[p->write_pos] = buf[i];
        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        sem_post(&p->sem_read);
    }
    return i;
}

int pipe_read(pipe_t* p, char* buf, int n) {
    int i;
    for (i = 0; i < n; i++) {
        sem_wait(&p->sem_read);
        // 调度禁用时 sem_wait 不阻塞，需检查实际是否有数据
        if (p->read_pos == p->write_pos) {
            break;  // 缓冲为空
        }
        buf[i] = p->buffer[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        sem_post(&p->sem_write);
    }
    return i;
}

// ===== 消息队列 =====

void msgq_init(msgq_t* q) {
    q->head = NULL;
    q->tail = NULL;
    sem_init(&q->sem_recv, 0);
    spin_init(&q->lock);
}

int msgq_send(msgq_t* q, const void* msg, int len) {
    int copy_len = len < MSG_SIZE ? len : MSG_SIZE;
    int i;
    msg_node_t* node = (msg_node_t*)kmalloc(sizeof(msg_node_t));
    if (!node) {
        printk("msgq_send: kmalloc failed\n");
        return -1;
    }
    char* dst = node->data;
    const char* src = (const char*)msg;
    for (i = 0; i < copy_len; i++) dst[i] = src[i];
    node->len = copy_len;
    node->next = NULL;

    spin_lock(&q->lock);
    if (q->tail) {
        q->tail->next = node;
        q->tail = node;
    } else {
        q->head = node;
        q->tail = node;
    }
    spin_unlock(&q->lock);

    sem_post(&q->sem_recv);
    return copy_len;
}

int msgq_recv(msgq_t* q, void* buf, int len) {
    sem_wait(&q->sem_recv);

    spin_lock(&q->lock);
    if (!q->head) {
        // 调度禁用时 sem_wait 不阻塞，队列可能仍为空
        spin_unlock(&q->lock);
        return 0;
    }
    msg_node_t* node = q->head;
    q->head = node->next;
    if (!q->head) q->tail = NULL;
    spin_unlock(&q->lock);

    int copy_len = node->len < len ? node->len : len;
    int i;
    char* dst = (char*)buf;
    const char* src = node->data;
    for (i = 0; i < copy_len; i++) dst[i] = src[i];
    kfree(node);
    return copy_len;
}

// ===== 共享内存 =====

int shm_create(shm_t* shm, int pages) {
    if (!shm || pages <= 0) return -1;

    // 简化：多页假设物理连续，仅记录第一页地址
    uint32_t phys = pmm_alloc_page();
    if (!phys) {
        printk("shm_create: pmm_alloc_page failed\n");
        return -1;
    }
    shm->phys_addr = phys;
    shm->size = pages * 4096;

    int i;
    for (i = 1; i < pages; i++) {
        pmm_alloc_page();
    }
    return 0;
}

int shm_map(shm_t* shm, uint32_t* vaddr) {
    if (!shm || !vaddr || !shm->phys_addr) return -1;

    uint32_t virt = shm_next_vaddr;
    int pages = shm->size / 4096;
    int i;
    for (i = 0; i < pages; i++) {
        if (!map_page(virt + i * 4096, shm->phys_addr + i * 4096,
                      PAGE_PRESENT | PAGE_WRITE)) {
            printk("shm_map: map_page failed at offset %d\n", i);
            return -1;
        }
    }
    shm_next_vaddr += pages * 4096;
    *vaddr = virt;
    return 0;
}
