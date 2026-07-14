#ifndef EVENT_H
#define EVENT_H

#include <stdint.h>

#define FD_READ   1
#define FD_WRITE  2
#define FD_EXCEPT 4

#define EVENT_NFDS_MAX 256
#define EVENT_TIMEOUT_INF ((uint32_t)0xFFFFFFFFu)

typedef struct {
    int fd;
    int events;
    int revents;
} pollfd_t;

int sys_select(int nfds, uint32_t* readfds, uint32_t* writefds,
               uint32_t* exceptfds, uint32_t timeout_ms);
int sys_poll(pollfd_t* fds, int nfds, uint32_t timeout_ms);

int sys_epoll_create(void);
int sys_epoll_ctl(int epfd, int op, int fd, void* event);
int sys_epoll_wait(int epfd, void* events, int maxevents, int timeout_ms);

void event_init(void);

#endif
