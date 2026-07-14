#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdint.h>

#define NOTIFY_LEVEL_INFO    0
#define NOTIFY_LEVEL_SUCCESS 1
#define NOTIFY_LEVEL_ERROR   2
#define NOTIFY_LEVEL_WARNING 3

#define NOTIFY_MAX 4
#define NOTIFY_TITLE_SIZE 64
#define NOTIFY_BODY_SIZE  128

typedef struct {
    char title[NOTIFY_TITLE_SIZE];
    char body[NOTIFY_BODY_SIZE];
    int level;
    uint32_t start_tick;
    int active;
} notify_t;

void notify_init(void);
void notify_send(const char* title, const char* body, int level);
void notify_render(void);
void notify_update(void);

#endif
