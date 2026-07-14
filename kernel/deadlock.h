#ifndef DEADLOCK_H
#define DEADLOCK_H

#include <stdint.h>

// 记录 from_pid 等待 to_pid
void deadlock_record_wait(int from_pid, int to_pid);

// 清除 pid 相关的所有等待边
void deadlock_clear_wait(int pid);

// 检测等待图中的环，返回环中任一 pid，无环返回 -1
int deadlock_detect(void);

// 清空整个等待图
void deadlock_reset(void);

#endif
