#include "event.h"
#include "pipe.h"
#include "tty.h"
#include "fat32.h"
#include "sched.h"
#include "idt.h"
#include "syscall.h"
#include "printk.h"
#include <stdint.h>

static int fd_readable(int fd) {
    if (fd == 0) {
        tty_t* t = tty_get(tty_get_current());
        if (!t) return 0;
        if (t->canonical) return t->line_ready;
        return t->in_head != t->in_tail;
    }
    if (fd >= PIPE_FD_BASE && fd < PIPE_FD_END) {
        return pipe_fd_readable(fd);
    }
    if (fd >= 100) {
        return 0;
    }
    if (fd >= 3 && fd < MAX_FD) return 1;
    return 0;
}

static int fd_writable(int fd) {
    if (fd == 1 || fd == 2) return 1;
    if (fd >= PIPE_FD_BASE && fd < PIPE_FD_END) {
        return pipe_fd_writable(fd);
    }
    if (fd >= 100) {
        return 0;
    }
    if (fd >= 3 && fd < MAX_FD) return 1;
    return 0;
}

static int fd_check_events(int fd, int events) {
    int revents = 0;
    if (events & FD_READ) {
        if (fd_readable(fd)) revents |= FD_READ;
    }
    if (events & FD_WRITE) {
        if (fd_writable(fd)) revents |= FD_WRITE;
    }
    return revents;
}

static int bitmap_get(uint32_t* fds, int fd) {
    if (!fds) return 0;
    return (fds[fd >> 5] >> (fd & 31)) & 1u;
}

static void bitmap_clear(uint32_t* fds, int fd) {
    if (!fds) return;
    fds[fd >> 5] &= ~(1u << (fd & 31));
}

static int select_has_ready(int nfds, uint32_t* readfds, uint32_t* writefds, uint32_t* exceptfds) {
    int fd;
    for (fd = 0; fd < nfds; fd++) {
        int events = 0;
        if (bitmap_get(readfds, fd)) events |= FD_READ;
        if (bitmap_get(writefds, fd)) events |= FD_WRITE;
        if (bitmap_get(exceptfds, fd)) events |= FD_EXCEPT;
        if (!events) continue;
        if (fd_check_events(fd, events)) return 1;
    }
    return 0;
}

static int select_harvest(int nfds, uint32_t* readfds, uint32_t* writefds, uint32_t* exceptfds) {
    int fd;
    int ready_count = 0;
    for (fd = 0; fd < nfds; fd++) {
        int want_read = bitmap_get(readfds, fd);
        int want_write = bitmap_get(writefds, fd);
        int want_except = bitmap_get(exceptfds, fd);
        if (!want_read && !want_write && !want_except) continue;

        int events = 0;
        if (want_read) events |= FD_READ;
        if (want_write) events |= FD_WRITE;
        if (want_except) events |= FD_EXCEPT;

        int revents = fd_check_events(fd, events);

        if (want_read) {
            if (revents & FD_READ) ready_count++;
            else bitmap_clear(readfds, fd);
        }
        if (want_write) {
            if (revents & FD_WRITE) ready_count++;
            else bitmap_clear(writefds, fd);
        }
        if (want_except) {
            bitmap_clear(exceptfds, fd);
        }
    }
    return ready_count;
}

int sys_select(int nfds, uint32_t* readfds, uint32_t* writefds,
               uint32_t* exceptfds, uint32_t timeout_ms) {
    if (nfds < 0 || nfds > EVENT_NFDS_MAX) return -EINVAL;

    if (!select_has_ready(nfds, readfds, writefds, exceptfds)) {
        if (timeout_ms == 0) return 0;

        if (timeout_ms == EVENT_TIMEOUT_INF) {
            while (!select_has_ready(nfds, readfds, writefds, exceptfds)) {
                schedule();
            }
        } else {
            uint32_t start = timer_ticks;
            uint32_t target = timeout_ms / 10;
            if (target == 0) target = 1;
            while (timer_ticks - start < target) {
                if (select_has_ready(nfds, readfds, writefds, exceptfds)) break;
                schedule();
            }
        }
    }

    return select_harvest(nfds, readfds, writefds, exceptfds);
}

int sys_poll(pollfd_t* fds, int nfds, uint32_t timeout_ms) {
    if (!fds || nfds < 0 || nfds > EVENT_NFDS_MAX) return -EINVAL;

    int i;
    for (i = 0; i < nfds; i++) fds[i].revents = 0;

    int has_ready = 0;
    for (i = 0; i < nfds; i++) {
        if (fd_check_events(fds[i].fd, fds[i].events)) {
            has_ready = 1;
            break;
        }
    }

    if (!has_ready) {
        if (timeout_ms == 0) return 0;

        if (timeout_ms == EVENT_TIMEOUT_INF) {
            while (1) {
                for (i = 0; i < nfds; i++) {
                    if (fd_check_events(fds[i].fd, fds[i].events)) {
                        has_ready = 1;
                        break;
                    }
                }
                if (has_ready) break;
                schedule();
            }
        } else {
            uint32_t start = timer_ticks;
            uint32_t target = timeout_ms / 10;
            if (target == 0) target = 1;
            while (timer_ticks - start < target) {
                for (i = 0; i < nfds; i++) {
                    if (fd_check_events(fds[i].fd, fds[i].events)) {
                        has_ready = 1;
                        break;
                    }
                }
                if (has_ready) break;
                schedule();
            }
        }
    }

    int ready_count = 0;
    for (i = 0; i < nfds; i++) {
        fds[i].revents = fd_check_events(fds[i].fd, fds[i].events);
        if (fds[i].revents) ready_count++;
    }
    return ready_count;
}

int sys_epoll_create(void) {
    return -ENOSYS;
}

int sys_epoll_ctl(int epfd, int op, int fd, void* event) {
    (void)epfd; (void)op; (void)fd; (void)event;
    return -ENOSYS;
}

int sys_epoll_wait(int epfd, void* events, int maxevents, int timeout_ms) {
    (void)epfd; (void)events; (void)maxevents; (void)timeout_ms;
    return -ENOSYS;
}

void event_init(void) {
    printk("event: select/poll initialized (nfds max %d)\n", EVENT_NFDS_MAX);
}
