/*
 * NEO libc - unistd：POSIX 风格封装
 *
 * 这些函数为弱符号，调用原始 neo_ 桩并通过 neo_errno_check 设置 errno
 * （负返回转 -1 + errno），符合 POSIX 返回约定，可在应用中覆盖。
 */

#include "neolibc.h"

#define WRAP_LONG(call)        (neo_errno_check((long)(call)))
#define WRAP_INT(call)          ((int)neo_errno_check((long)(call)))

long write(int fd, const void* buf, size_t len)
{
    return WRAP_LONG(neo_write(fd, buf, len));
}

long read(int fd, void* buf, size_t len)
{
    return WRAP_LONG(neo_read(fd, buf, len));
}

long open(const char* path, int flags)
{
    return WRAP_LONG(neo_open(path, flags));
}

int close(int fd)
{
    return WRAP_INT(neo_close(fd));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)neo_errno_check(neo_lseek(fd, offset, whence));
}

pid_t fork(void)
{
    return (pid_t)neo_errno_check(neo_fork());
}

long exec(const char* path)
{
    return WRAP_LONG(neo_exec(path));
}

int dup(int fd)
{
    return WRAP_INT(neo_dup(fd));
}

int dup2(int oldfd, int newfd)
{
    return WRAP_INT(neo_dup2(oldfd, newfd));
}

int unlink(const char* path)
{
    return WRAP_INT(neo_unlink(path));
}

int mkdir(const char* path, int mode)
{
    return WRAP_INT(neo_mkdir(path, mode));
}

int rmdir(const char* path)
{
    return WRAP_INT(neo_rmdir(path));
}

int stat(const char* path, void* buf)
{
    return WRAP_INT(neo_stat(path, buf));
}

int pipe(int fds[2])
{
    return WRAP_INT(neo_pipe(fds));
}

int getpid(void)
{
    return (int)neo_getpid();
}

int getppid(void)
{
    return (int)neo_getppid();
}

pid_t wait(int* status)
{
    return waitpid(-1, status, 0);
}

pid_t waitpid(pid_t pid, int* status, int options)
{
    return (pid_t)neo_errno_check(neo_wait(pid, status, options));
}

int kill(pid_t pid, int sig)
{
    return WRAP_INT(neo_kill(pid, sig));
}

unsigned int sleep(unsigned int seconds)
{
    long r = neo_errno_check(neo_sleep(seconds));
    if (r < 0) return seconds;
    return 0;
}

int usleep(unsigned long usec)
{
    return WRAP_INT(neo_usleep(usec));
}

int chdir(const char* path)
{
    return WRAP_INT(neo_chdir(path));
}

char* getcwd(char* buf, size_t size)
{
    long r = neo_errno_check(neo_getcwd(buf, size));
    if (r < 0) return (char*)0;
    return buf;
}

int socket(int domain, int type, int protocol)
{
    return WRAP_INT(neo_socket(domain, type, protocol));
}

int connect(int fd, const void* addr, int addrlen)
{
    return WRAP_INT(neo_connect(fd, addr, addrlen));
}

int reboot(int magic)
{
    return WRAP_INT(neo_reboot(magic));
}

int poweroff(int magic)
{
    return WRAP_INT(neo_poweroff(magic));
}
