#ifndef KLOG_H
#define KLOG_H

#include <stdint.h>

#define KLOG_SIZE 8192
#define KLOG_ENTRY_MAX 256

#define KLOG_DEBUG 0
#define KLOG_INFO  1
#define KLOG_WARN  2
#define KLOG_ERROR 3
#define KLOG_FATAL 4

void klog_init(void);
void klog_write_level(int level, const char* fmt, ...);
void klog_write_str(const char* s);
int klog_read(char* buf, int max);
int klog_read_recent(char* buf, int max, int recent_bytes);
void klog_set_level(int level);
int  klog_get_level(void);

#define klog_write(fmt, ...) klog_write_level(KLOG_INFO, fmt, ##__VA_ARGS__)

#endif
