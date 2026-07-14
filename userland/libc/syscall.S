/*
 * NEO libc - 系统调用汇编桩（AT&T 语法，32 位 x86 保护模式）
 *
 * ABI（冻结，见 kernel/syscall.h）：
 *   - 触发：int $0x80
 *   - eax = 系统调用编号
 *   - ebx = arg1, ecx = arg2, edx = arg3, esi = arg4, edi = arg5
 *   - 返回值在 eax，负数表示错误
 *
 * 编译选项 -fno-leading-underscore：函数名前不加下划线。
 * 每个桩统一保存并恢复 ebx/ecx/edx/esi/edi 五个寄存器，故参数位于：
 *   arg1 = 24(%esp), arg2 = 28(%esp), arg3 = 32(%esp),
 *   arg4 = 36(%esp), arg5 = 40(%esp)
 * （返回地址位于 20(%esp)）。
 */

.text

.macro SYSCALL name, num
.global \name
.type   \name, @function
\name:
    pushl %ebx
    pushl %ecx
    pushl %edx
    pushl %esi
    pushl %edi
    movl  $\num, %eax
    movl  24(%esp), %ebx
    movl  28(%esp), %ecx
    movl  32(%esp), %edx
    movl  36(%esp), %esi
    movl  40(%esp), %edi
    int   $0x80
    popl  %edi
    popl  %esi
    popl  %edx
    popl  %ecx
    popl  %ebx
    ret
.endm

/* === 阶段四：基础系统调用（1-15） === */
SYSCALL neo_write,       1
SYSCALL neo_read,        2
SYSCALL neo_exit,        3
SYSCALL neo_yield,       4
SYSCALL neo_alloc,       5
SYSCALL neo_draw_frame,  6
SYSCALL neo_play_audio,  7
SYSCALL neo_fork,        8
SYSCALL neo_exec,        9
SYSCALL neo_open,       10
SYSCALL neo_close,      11
SYSCALL neo_getpid,     12
SYSCALL neo_wait,       13
SYSCALL neo_getuid,      14
SYSCALL neo_setuid,      15

/* === 文件系统操作（16-25） === */
SYSCALL neo_lseek,      16
SYSCALL neo_stat,       17
SYSCALL neo_mkdir,      18
SYSCALL neo_rmdir,      19
SYSCALL neo_unlink,     20
SYSCALL neo_dup,        21
SYSCALL neo_dup2,       22
SYSCALL neo_getcwd,     23
SYSCALL neo_chdir,      24
SYSCALL neo_readdir,    25

/* === 内存管理（26-30） === */
SYSCALL neo_mmap,       26
SYSCALL neo_munmap,     27
SYSCALL neo_brk,        28
SYSCALL neo_sbrk,       29
SYSCALL neo_mprotect,   30

/* === 进程与信号（31-40） === */
SYSCALL neo_kill,       31
SYSCALL neo_signal,     32
SYSCALL neo_sigaction,  33
SYSCALL neo_sigreturn,  34
SYSCALL neo_sigprocmask,35
SYSCALL neo_getppid,   36
SYSCALL neo_setpriority,37
SYSCALL neo_getpriority,38
SYSCALL neo_nice,       39
SYSCALL neo_pause,      40

/* === 时间（41-45） === */
SYSCALL neo_sleep,      41
SYSCALL neo_usleep,     42
SYSCALL neo_gettimeofday,43
SYSCALL neo_time,       44
SYSCALL neo_alarm,      45

/* === IPC 与管道（46-50） === */
SYSCALL neo_pipe,       46
SYSCALL neo_mq_open,    47
SYSCALL neo_mq_send,    48
SYSCALL neo_mq_recv,    49
SYSCALL neo_shmget,     50

/* === 网络与 Socket（51-60） === */
SYSCALL neo_socket,     51
SYSCALL neo_connect,    52
SYSCALL neo_bind,       53
SYSCALL neo_listen,     54
SYSCALL neo_accept,     55
SYSCALL neo_send,       56
SYSCALL neo_recv,       57
SYSCALL neo_sendto,     58
SYSCALL neo_recvfrom,   59
SYSCALL neo_shutdown,   60

/* === 设备与电源（61-65） === */
SYSCALL neo_ioctl,      61
SYSCALL neo_reboot,     62
SYSCALL neo_poweroff,   63
SYSCALL neo_gettime,    64
SYSCALL neo_settime,    65

/* === 事件与终端（66-70） === */
SYSCALL neo_select,     66
SYSCALL neo_poll,       67
SYSCALL neo_openpty,    68
SYSCALL neo_tcgetattr,  69
SYSCALL neo_tcsetattr,  70

.section note.GNU-stack,"",@progbits
