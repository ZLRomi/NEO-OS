#include "pipe.h"
#include "ipc.h"
#include "fat32.h"
#include "heap.h"
#include "printk.h"
#include "sync.h"
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int used;
    pipe_t* pipe;
    int is_write;
} pipe_fd_entry_t;

static pipe_fd_entry_t pipe_fd_table[PIPE_FD_MAX];

typedef struct {
    int used;
    char path[MAX_PATH];
    pipe_t* pipe;
    int readers;
    int writers;
} fifo_entry_t;

static fifo_entry_t fifo_table[FIFO_MAX];

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void str_copy(char* dst, const char* src) {
    int i = 0;
    while (src[i] && i < MAX_PATH - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int alloc_pipe_fd(void) {
    int i;
    for (i = 0; i < PIPE_FD_MAX; i++) {
        if (!pipe_fd_table[i].used) return PIPE_FD_BASE + i;
    }
    return -1;
}

int pipe_fd_is_pipe(int fd) {
    if (fd < PIPE_FD_BASE || fd >= PIPE_FD_END) return 0;
    return pipe_fd_table[fd - PIPE_FD_BASE].used;
}

int pipe_fd_readable(int fd) {
    if (!pipe_fd_is_pipe(fd)) return 0;
    pipe_fd_entry_t* e = &pipe_fd_table[fd - PIPE_FD_BASE];
    if (!e->used || e->is_write || !e->pipe) return 0;
    pipe_t* p = e->pipe;
    return p->read_pos != p->write_pos;
}

int pipe_fd_writable(int fd) {
    if (!pipe_fd_is_pipe(fd)) return 0;
    pipe_fd_entry_t* e = &pipe_fd_table[fd - PIPE_FD_BASE];
    if (!e->used || !e->is_write || !e->pipe) return 0;
    pipe_t* p = e->pipe;
    return (p->write_pos + 1) % PIPE_BUF_SIZE != p->read_pos;
}

int pipe_create(int* read_fd, int* write_fd) {
    if (!read_fd || !write_fd) return -EINVAL;
    pipe_t* p = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!p) return -ENOMEM;
    pipe_init(p);
    int rfd = alloc_pipe_fd();
    int wfd;
    if (rfd >= 0) {
        wfd = alloc_pipe_fd();
    } else {
        wfd = -1;
    }
    if (rfd < 0 || wfd < 0) {
        kfree(p);
        return -ENOSPC;
    }
    pipe_fd_table[rfd - PIPE_FD_BASE].used = 1;
    pipe_fd_table[rfd - PIPE_FD_BASE].pipe = p;
    pipe_fd_table[rfd - PIPE_FD_BASE].is_write = 0;
    pipe_fd_table[wfd - PIPE_FD_BASE].used = 1;
    pipe_fd_table[wfd - PIPE_FD_BASE].pipe = p;
    pipe_fd_table[wfd - PIPE_FD_BASE].is_write = 1;
    *read_fd = rfd;
    *write_fd = wfd;
    return 0;
}

int pipe_read_fd(int fd, void* buf, int len) {
    if (!pipe_fd_is_pipe(fd)) return -EBADF;
    pipe_fd_entry_t* e = &pipe_fd_table[fd - PIPE_FD_BASE];
    if (e->is_write) return -EBADF;
    return pipe_read(e->pipe, (char*)buf, len);
}

int pipe_write_fd(int fd, const void* buf, int len) {
    if (!pipe_fd_is_pipe(fd)) return -EBADF;
    pipe_fd_entry_t* e = &pipe_fd_table[fd - PIPE_FD_BASE];
    if (!e->is_write) return -EBADF;
    return pipe_write(e->pipe, (const char*)buf, len);
}

static int pipe_has_other_ref(pipe_t* p, int skip_idx) {
    int i;
    for (i = 0; i < PIPE_FD_MAX; i++) {
        if (i == skip_idx) continue;
        if (pipe_fd_table[i].used && pipe_fd_table[i].pipe == p) return 1;
    }
    return 0;
}

static int pipe_is_fifo_owned(pipe_t* p) {
    int i;
    for (i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].used && fifo_table[i].pipe == p) return 1;
    }
    return 0;
}

int pipe_close(int fd) {
    if (!pipe_fd_is_pipe(fd)) return -EBADF;
    pipe_fd_entry_t* e = &pipe_fd_table[fd - PIPE_FD_BASE];
    pipe_t* p = e->pipe;
    e->used = 0;
    e->pipe = 0;
    if (!pipe_has_other_ref(p, fd - PIPE_FD_BASE) && !pipe_is_fifo_owned(p)) {
        kfree(p);
    }
    return 0;
}

int fifo_create(const char* path) {
    if (!path) return -EINVAL;
    int i;
    for (i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].used && str_eq(fifo_table[i].path, path)) {
            return -EEXIST;
        }
    }
    for (i = 0; i < FIFO_MAX; i++) {
        if (!fifo_table[i].used) {
            fat32_create(path);
            fifo_table[i].used = 1;
            str_copy(fifo_table[i].path, path);
            fifo_table[i].pipe = (pipe_t*)kmalloc(sizeof(pipe_t));
            if (!fifo_table[i].pipe) {
                fifo_table[i].used = 0;
                return -ENOMEM;
            }
            pipe_init(fifo_table[i].pipe);
            fifo_table[i].readers = 0;
            fifo_table[i].writers = 0;
            return 0;
        }
    }
    return -ENOMEM;
}

static fifo_entry_t* fifo_find(const char* path) {
    int i;
    for (i = 0; i < FIFO_MAX; i++) {
        if (fifo_table[i].used && str_eq(fifo_table[i].path, path)) {
            return &fifo_table[i];
        }
    }
    return 0;
}

int fifo_open(const char* path, int flags) {
    if (!path) return -EINVAL;
    fifo_entry_t* f = fifo_find(path);
    if (!f) {
        if (fifo_create(path) < 0) return -ENOENT;
        f = fifo_find(path);
        if (!f) return -ENOENT;
    }
    int is_write = (flags == FIFO_O_WRONLY);
    int fd = alloc_pipe_fd();
    if (fd < 0) return -ENOSPC;
    pipe_fd_table[fd - PIPE_FD_BASE].used = 1;
    pipe_fd_table[fd - PIPE_FD_BASE].pipe = f->pipe;
    pipe_fd_table[fd - PIPE_FD_BASE].is_write = is_write;
    if (is_write) {
        f->writers++;
    } else {
        f->readers++;
    }
    return fd;
}

int sys_dup(int oldfd) {
    if (oldfd < 0) return -EBADF;
    if (oldfd >= PIPE_FD_BASE && oldfd < PIPE_FD_END) {
        pipe_fd_entry_t* e = &pipe_fd_table[oldfd - PIPE_FD_BASE];
        if (!e->used) return -EBADF;
        int newfd = alloc_pipe_fd();
        if (newfd < 0) return -ENOSPC;
        pipe_fd_entry_t* n = &pipe_fd_table[newfd - PIPE_FD_BASE];
        n->used = 1;
        n->pipe = e->pipe;
        n->is_write = e->is_write;
        return newfd;
    }
    if (oldfd >= 0 && oldfd < MAX_FD) {
        return fat32_dup(oldfd);
    }
    return -EBADF;
}

int sys_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || newfd < 0) return -EBADF;
    if (oldfd == newfd) return newfd;

    if (oldfd >= PIPE_FD_BASE && oldfd < PIPE_FD_END) {
        if (newfd < PIPE_FD_BASE || newfd >= PIPE_FD_END) return -EBADF;
        pipe_close(newfd);
        pipe_fd_entry_t* e = &pipe_fd_table[oldfd - PIPE_FD_BASE];
        if (!e->used) return -EBADF;
        pipe_fd_entry_t* n = &pipe_fd_table[newfd - PIPE_FD_BASE];
        n->used = 1;
        n->pipe = e->pipe;
        n->is_write = e->is_write;
        return newfd;
    }

    if (oldfd >= 0 && oldfd < MAX_FD) {
        if (newfd >= PIPE_FD_BASE && newfd < PIPE_FD_END) {
            pipe_close(newfd);
        } else if (newfd >= 0 && newfd < MAX_FD) {
            fat32_close(newfd);
        } else {
            return -EBADF;
        }
        return fat32_dup2(oldfd, newfd);
    }

    return -EBADF;
}
