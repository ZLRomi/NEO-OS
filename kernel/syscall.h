#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

/*
 * NEO-OS System Call ABI (FROZEN)
 *
 * 稳定性约束:
 * - 已分配编号一旦发布即冻结，永不更改或复用
 * - 新增编号只能追加到列表末尾，不得插入或重排
 * - 参数布局保持 eax=编号, ebx=arg1, ecx=arg2, edx=arg3, esi=arg4, edi=arg5
 * - 返回值放在 eax，负数表示错误（-EINVAL/-ENOMEM/-EACCES 等）
 * - 用户态通过 int 0x80 触发，IDT 0x80 门 DPL=3
 *
 * 应用通过 NEO libc 封装调用，ABI 向后兼容已编译的 .neap 包
 */

/* === 阶段四：基础系统调用（已冻结，编号 1-15） === */
#define SYS_WRITE       1
#define SYS_READ        2
#define SYS_EXIT        3
#define SYS_YIELD       4
#define SYS_ALLOC       5
#define SYS_DRAW_FRAME  6
#define SYS_PLAY_AUDIO  7
#define SYS_FORK        8
#define SYS_EXEC        9
#define SYS_OPEN       10
#define SYS_CLOSE      11
#define SYS_GETPID     12
#define SYS_WAIT       13
#define SYS_GETUID     14
#define SYS_SETUID     15

/* === 文件系统操作（编号 16-25） === */
#define SYS_LSEEK      16
#define SYS_STAT       17
#define SYS_MKDIR      18
#define SYS_RMDIR      19
#define SYS_UNLINK     20
#define SYS_DUP        21
#define SYS_DUP2       22
#define SYS_GETCWD     23
#define SYS_CHDIR      24
#define SYS_READDIR    25

/* === 内存管理（编号 26-30） === */
#define SYS_MMAP       26
#define SYS_MUNMAP     27
#define SYS_BRK        28
#define SYS_SBRK       29
#define SYS_MPROTECT   30

/* === 进程与信号（编号 31-40） === */
#define SYS_KILL       31
#define SYS_SIGNAL     32
#define SYS_SIGACTION  33
#define SYS_SIGRETURN  34
#define SYS_SIGPROCMASK 35
#define SYS_GETPPID    36
#define SYS_SETPRIORITY 37
#define SYS_GETPRIORITY 38
#define SYS_NICE       39
#define SYS_PAUSE      40

/* === 时间（编号 41-45） === */
#define SYS_SLEEP      41
#define SYS_USLEEP     42
#define SYS_GETTIMEOFDAY 43
#define SYS_TIME       44
#define SYS_ALARM      45

/* === IPC 与管道（编号 46-50） === */
#define SYS_PIPE       46
#define SYS_MQ_OPEN    47
#define SYS_MQ_SEND    48
#define SYS_MQ_RECV    49
#define SYS_SHMGET     50

/* === 网络与 Socket（编号 51-60） === */
#define SYS_SOCKET     51
#define SYS_CONNECT    52
#define SYS_BIND       53
#define SYS_LISTEN     54
#define SYS_ACCEPT     55
#define SYS_SEND       56
#define SYS_RECV       57
#define SYS_SENDTO     58
#define SYS_RECVFROM   59
#define SYS_SHUTDOWN   60

/* === 设备与电源（编号 61-65） === */
#define SYS_IOCTL      61
#define SYS_REBOOT     62
#define SYS_POWEROFF   63
#define SYS_GETTIME    64
#define SYS_SETTIME    65

/* === 事件与终端（编号 66-70） === */
#define SYS_SELECT     66
#define SYS_POLL       67
#define SYS_OPENPTY    68
#define SYS_TCGETATTR  69
#define SYS_TCSETATTR  70

/* === 错误码 === */
#define EPERM           1
#define ENOENT          2
#define EINTR           4
#define EIO             5
#define EBADF           9
#define EAGAIN         11
#define ENOMEM         12
#define EACCES         13
#define EFAULT         14
#define EBUSY          16
#define EEXIST         17
#define ENOTDIR        20
#define EISDIR         21
#define EINVAL         22
#define ENOSPC         28
#define EPIPE          32
#define ENOSYS         38
#define ENOTCONN       57
#define ETIMEDOUT     110
#define ECONNREFUSED  111
#define ESRCH           3
#define EAFNOSUPPORT   97
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95

#define SYS_MAX        70

void syscall_handler(void* frame);
void syscall_init(void);

#endif
