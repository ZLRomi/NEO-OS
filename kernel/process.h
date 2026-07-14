#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "signal.h"

#define MAX_PROCESSES 64

// 进程状态
typedef enum {
    PROCESS_READY = 0,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_TERMINATED
} process_state_t;

// 进程控制块
typedef struct process {
    int pid;
    int state;
    int priority;          // 0 最高
    int time_slice;        // 剩余时间片，默认 10
    uint32_t page_dir;     // 页目录物理地址（CR3 值）
    uint32_t regs[8];      // eax, ebx, ecx, edx, esi, edi, ebp, esp
    uint32_t eip;
    uint32_t eflags;
    uint32_t kernel_stack;  // 内核栈顶
    int uid;                // 所属用户 id
    char name[16];
    struct process* next;   // 队列链接指针
    uint32_t signal_handlers[NSIG];
    uint32_t pending_signals;
    uint32_t signal_mask;
    int parent_pid;
} process_t;

void process_init(void);
int process_create(uint32_t entry, int priority, const char* name);
int process_fork_current(void);
void process_exit(int pid);
void process_block(int state);
void process_wake(int pid);
process_t* process_get_current(void);
process_t* process_get_by_pid(int pid);
int process_get_all(process_t** out, int max);
void switch_to(process_t* prev, process_t* next);

// 供调度器使用的队列接口
void process_set_current(process_t* p);
process_t* process_ready_head(void);
void process_ready_enqueue(process_t* p);
void process_ready_remove(process_t* p);

// 从 cur 之后遍历进程表，找下一个 READY 进程（round-robin，回绕）
process_t* process_get_next_ready_after(process_t* cur);

#endif
