#ifndef NEOLIBC_H
#define NEOLIBC_H

/*
 * NEO libc - 用户态标准库公共头文件
 * 适用于 NEO-OS 32 位 x86 保护模式，通过 int 0x80 系统调用进入内核。
 */

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef long        ssize_t;
typedef long        off_t;
typedef int         pid_t;
typedef int         fd_t;
#endif /* __ASSEMBLER__ */

/* 标准文件描述符 */
#define NEO_STDIN    0
#define NEO_STDOUT   1
#define NEO_STDERR   2

/* lseek 起点 */
#define SEEK_SET     0
#define SEEK_CUR     1
#define SEEK_END     2

/* open 标志 */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0x40
#define O_TRUNC      0x80
#define O_APPEND     0x100
#define O_EXCL       0x200

#ifndef __ASSEMBLER__
/* ===================================================================== */
/* 系统调用原始桩（实现在 syscall.S，返回原始 eax，负数表示错误）        */
/* ===================================================================== */
long  neo_write(int fd, const void* buf, size_t len);
long  neo_read(int fd, void* buf, size_t len);
void  neo_exit(int code) __attribute__((noreturn));
long  neo_yield(void);
void* neo_alloc(int nbytes);
long  neo_draw_frame(void* buf);
long  neo_play_audio(const void* buf, int len);
long  neo_fork(void);
long  neo_exec(const char* path);
long  neo_open(const char* path, int flags);
long  neo_close(int fd);
long  neo_getpid(void);
long  neo_wait(int pid, int* status, int options);
long  neo_getuid(void);
long  neo_setuid(int uid);
long  neo_lseek(int fd, off_t offset, int whence);
long  neo_stat(const char* path, void* statbuf);
long  neo_mkdir(const char* path, int mode);
long  neo_rmdir(const char* path);
long  neo_unlink(const char* path);
long  neo_dup(int fd);
long  neo_dup2(int oldfd, int newfd);
long  neo_getcwd(char* buf, size_t size);
long  neo_chdir(const char* path);
long  neo_readdir(int fd, void* buf, int index);
void* neo_mmap(void* addr, size_t len, int prot, int flags, int fd);
long  neo_munmap(void* addr, size_t len);
long  neo_brk(void* addr);
void* neo_sbrk(long inc);
long  neo_mprotect(void* addr, size_t len, int prot);
long  neo_kill(int pid, int sig);
long  neo_signal(int sig, void* handler);
long  neo_sigaction(int sig, const void* act, void* oldact);
long  neo_sigreturn(void);
long  neo_sigprocmask(int how, const void* set, void* oldset);
long  neo_getppid(void);
long  neo_setpriority(int which, int who, int prio);
long  neo_getpriority(int which, int who);
long  neo_nice(int inc);
long  neo_pause(void);
long  neo_sleep(unsigned int seconds);
long  neo_usleep(unsigned long usec);
long  neo_gettimeofday(void* tv, void* tz);
long  neo_time(long* tloc);
long  neo_alarm(unsigned int seconds);
long  neo_pipe(int fds[2]);
long  neo_mq_open(const char* name, int oflag, int mode, void* attr);
long  neo_mq_send(int mqd, const char* msg, size_t len, unsigned int prio);
long  neo_mq_recv(int mqd, char* msg, size_t len, unsigned int* prio);
long  neo_shmget(int key, size_t size, int flags);
long  neo_socket(int domain, int type, int protocol);
long  neo_connect(int fd, const void* addr, int addrlen);
long  neo_bind(int fd, const void* addr, int addrlen);
long  neo_listen(int fd, int backlog);
long  neo_accept(int fd, void* addr, int* addrlen);
long  neo_send(int fd, const void* buf, size_t len, int flags);
long  neo_recv(int fd, void* buf, size_t len, int flags);
long  neo_sendto(int fd, const void* buf, size_t len, int flags, const void* addr);
long  neo_recvfrom(int fd, void* buf, size_t len, int flags, void* addr);
long  neo_shutdown(int fd, int how);
long  neo_ioctl(int fd, int cmd, long arg);
long  neo_reboot(int magic);
long  neo_poweroff(int magic);
long  neo_gettime(void* t);
long  neo_settime(const void* t);
long  neo_select(int nfds, void* rfds, void* wfds, void* efds, void* timeout);
long  neo_poll(void* fds, unsigned long nfds, int timeout);
long  neo_openpty(int* master, int* slave, char* name);
long  neo_tcgetattr(int fd, void* termios);
long  neo_tcsetattr(int fd, int act, const void* termios);

/* ===================================================================== */
/* errno C 封装层（syscalls.c）                                          */
/* ===================================================================== */
extern int neo_errno;

long        neo_errno_check(long ret);
const char* neo_strerror(int err);
void        neo_perror(const char* s);

/* errno 访问宏，模仿标准 errno 语义 */
#define errno   neo_errno

/* ===================================================================== */
/* 字符串/内存函数（string.c）                                          */
/* ===================================================================== */
size_t  neo_strlen(const char* s);
char*   neo_strcpy(char* dst, const char* src);
char*   neo_strncpy(char* dst, const char* src, size_t n);
char*   neo_strcat(char* dst, const char* src);
int     neo_strcmp(const char* a, const char* b);
int     neo_strncmp(const char* a, const char* b, size_t n);
char*   neo_strchr(const char* s, int c);
void*   neo_memcpy(void* dst, const void* src, size_t n);
void*   neo_memset(void* dst, int c, size_t n);
int     neo_memcmp(const void* a, const void* b, size_t n);
void*   neo_memmove(void* dst, const void* src, size_t n);

/* POSIX 风格别名（弱符号，实现在对应 .c 文件中，可在应用中覆盖） */
size_t  strlen(const char* s);
char*   strcpy(char* dst, const char* src);
char*   strncpy(char* dst, const char* src, size_t n);
char*   strcat(char* dst, const char* src);
int     strcmp(const char* a, const char* b);
int     strncmp(const char* a, const char* b, size_t n);
char*   strchr(const char* s, int c);
void*   memcpy(void* dst, const void* src, size_t n);
void*   memset(void* dst, int c, size_t n);
int     memcmp(const void* a, const void* b, size_t n);
void*   memmove(void* dst, const void* src, size_t n);

/* ===================================================================== */
/* stdlib（stdlib.c）                                                   */
/* ===================================================================== */
void*   neo_malloc(size_t size);
void    neo_free(void* ptr);
void*   neo_calloc(size_t n, size_t sz);
int     neo_atoi(const char* s);
char*   neo_itoa(long value, char* str, int base);

/* exit 由 stdlib 提供，调用 neo_exit 汇编桩 */
void    exit(int code) __attribute__((noreturn));
void    _exit(int code) __attribute__((noreturn));
int     atoi(const char* s);
char*   itoa(long value, char* str, int base);
void*   malloc(size_t size);
void    free(void* ptr);
void*   calloc(size_t n, size_t sz);

/* ===================================================================== */
/* stdio（stdio.c）                                                     */
/* ===================================================================== */
int     neo_vfprintf(int fd, const char* fmt, va_list ap);
int     neo_vprintf(const char* fmt, va_list ap);
int     neo_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int     neo_fprintf(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int     neo_putchar(int c);
int     neo_puts(const char* s);

int     printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
int     vprintf(const char* fmt, va_list ap);
int     fprintf(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
int     putchar(int c);
int     puts(const char* s);

/* ===================================================================== */
/* unistd POSIX 风格封装（unistd.c）                                    */
/* 这些函数为弱符号，调用原始 neo_ 桩并设置 errno                        */
/* ===================================================================== */
long    write(int fd, const void* buf, size_t len);
long    read(int fd, void* buf, size_t len);
long    open(const char* path, int flags);
int     close(int fd);
off_t   lseek(int fd, off_t offset, int whence);
pid_t   fork(void);
long    exec(const char* path);
int     dup(int fd);
int     dup2(int oldfd, int newfd);
int     unlink(const char* path);
int     mkdir(const char* path, int mode);
int     rmdir(const char* path);
int     stat(const char* path, void* buf);
int     pipe(int fds[2]);
int     getpid(void);
int     getppid(void);
pid_t   wait(int* status);
pid_t   waitpid(pid_t pid, int* status, int options);
int     kill(pid_t pid, int sig);
unsigned int sleep(unsigned int seconds);
int     usleep(unsigned long usec);
int     chdir(const char* path);
char*   getcwd(char* buf, size_t size);
int     socket(int domain, int type, int protocol);
int     connect(int fd, const void* addr, int addrlen);
int     reboot(int magic);
int     poweroff(int magic);

#endif /* __ASSEMBLER__ */
#endif /* NEOLIBC_H */
