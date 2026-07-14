#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_FD_BASE 16
#define PIPE_FD_MAX 16
#define PIPE_FD_END (PIPE_FD_BASE + PIPE_FD_MAX)

#define FIFO_PREFIX "fifo:"
#define FIFO_MAX 8

#define FIFO_O_RDONLY 0
#define FIFO_O_WRONLY 1

int pipe_create(int* read_fd, int* write_fd);
int pipe_read_fd(int fd, void* buf, int len);
int pipe_write_fd(int fd, const void* buf, int len);
int pipe_close(int fd);

int fifo_create(const char* path);
int fifo_open(const char* path, int flags);

int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);

int pipe_fd_is_pipe(int fd);
int pipe_fd_readable(int fd);
int pipe_fd_writable(int fd);

#endif
