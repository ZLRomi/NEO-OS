#ifndef IPC_H
#define IPC_H

#include <stdint.h>
#include "sync.h"

#define PIPE_BUF_SIZE 4096
#define MSG_SIZE 64

// 管道：环形缓冲
typedef struct {
    char buffer[PIPE_BUF_SIZE];
    volatile int read_pos;
    volatile int write_pos;
    semaphore_t sem_read;   // 可读数据量
    semaphore_t sem_write;  // 可写空闲量
} pipe_t;

// 消息队列节点
typedef struct msg_node {
    char data[MSG_SIZE];
    int len;
    struct msg_node* next;
} msg_node_t;

// 消息队列
typedef struct {
    msg_node_t* head;
    msg_node_t* tail;
    semaphore_t sem_recv;   // 可接收消息数
    spinlock_t lock;        // 保护链表
} msgq_t;

// 共享内存
typedef struct {
    uint32_t phys_addr;
    uint32_t size;
} shm_t;

// 管道接口
void pipe_init(pipe_t* p);
int pipe_read(pipe_t* p, char* buf, int n);
int pipe_write(pipe_t* p, const char* buf, int n);

// 消息队列接口
void msgq_init(msgq_t* q);
int msgq_send(msgq_t* q, const void* msg, int len);
int msgq_recv(msgq_t* q, void* buf, int len);

// 共享内存接口
int shm_create(shm_t* shm, int pages);
int shm_map(shm_t* shm, uint32_t* vaddr);

#endif
