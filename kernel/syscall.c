#include "syscall.h"
#include "serial.h"
#include "process.h"
#include "sched.h"
#include "memory.h"
#include "fat32.h"
#include "socket.h"
#include "signal.h"
#include "power.h"
#include "idt.h"
#include "pipe.h"
#include "tty.h"
#include "event.h"
#include "gfx_driver.h"
#include <stdint.h>

extern uint32_t fb_width, fb_height, fb_pitch;

__asm__(
    ".global syscall_handler\n"
    "syscall_handler:\n\t"
    "pusha\n\t"
    "push %ds\n\t"
    "push %es\n\t"
    "push %fs\n\t"
    "mov $0x10, %ax\n\t"
    "mov %ax, %ds\n\t"
    "mov %ax, %es\n\t"
    "mov %ax, %fs\n\t"
    "mov %esp, %eax\n\t"
    "add $12, %eax\n\t"
    "push %eax\n\t"
    "call syscall_dispatch\n\t"
    "add $4, %esp\n\t"
    "pop %fs\n\t"
    "pop %es\n\t"
    "pop %ds\n\t"
    "popa\n\t"
    "iret\n\t"
);

static int sys_write(int fd, const char* buf, int len);
static int sys_read(int fd, char* buf, int len);
static void sys_exit(int code);
static void sys_yield(void);
static uint32_t sys_alloc(int nbytes);
static int sys_draw_frame(uint8_t* buf);
static int sys_play_audio(const void* buf, int len);
static int sys_fork(void);
static int sys_exec(const char* path);
static int sys_open(const char* path, int flags);
static int sys_close(int fd);
static int sys_getpid(void);
static int sys_sleep(unsigned int seconds);
static int sys_usleep(unsigned int usec);
static int sys_gettimeofday(void* tv, void* tz);

void syscall_dispatch(uint32_t* regs) {
    uint32_t num  = regs[7];
    uint32_t arg1 = regs[4];
    uint32_t arg2 = regs[6];
    uint32_t arg3 = regs[5];
    uint32_t arg4 = regs[3];
    uint32_t arg5 = regs[2];
    uint32_t ret = 0;

    switch (num) {
        case SYS_WRITE:        ret = sys_write((int)arg1, (const char*)arg2, (int)arg3); break;
        case SYS_READ:         ret = sys_read((int)arg1, (char*)arg2, (int)arg3); break;
        case SYS_EXIT:         sys_exit((int)arg1); break;
        case SYS_YIELD:        sys_yield(); ret = 0; break;
        case SYS_ALLOC:        ret = sys_alloc((int)arg1); break;
        case SYS_DRAW_FRAME:   ret = sys_draw_frame((uint8_t*)arg1); break;
        case SYS_PLAY_AUDIO:   ret = sys_play_audio((const void*)arg1, (int)arg2); break;
        case SYS_FORK:         ret = sys_fork(); break;
        case SYS_EXEC:         ret = sys_exec((const char*)arg1); break;
        case SYS_OPEN:         ret = sys_open((const char*)arg1, (int)arg2); break;
        case SYS_CLOSE:        ret = sys_close((int)arg1); break;
        case SYS_GETPID:       ret = sys_getpid(); break;
        case SYS_PIPE: {
            int* fds = (int*)arg1;
            int rfd = 0, wfd = 0;
            int r = pipe_create(&rfd, &wfd);
            if (r < 0) ret = (uint32_t)r;
            else { fds[0] = rfd; fds[1] = wfd; ret = 0; }
            break;
        }
        case SYS_DUP:          ret = sys_dup((int)arg1); break;
        case SYS_DUP2:         ret = sys_dup2((int)arg1, (int)arg2); break;
        case SYS_KILL:         ret = sys_kill((int)arg1, (int)arg2); break;
        case SYS_SIGNAL:       ret = sys_signal((int)arg1, arg2); break;
        case SYS_SIGACTION:    ret = sys_sigaction((int)arg1, (const void*)arg2, (void*)arg3); break;
        case SYS_SIGPROCMASK:  ret = sys_sigprocmask((int)arg1, (const uint32_t*)arg2, (uint32_t*)arg3); break;
        case SYS_SLEEP:        ret = sys_sleep((unsigned int)arg1); break;
        case SYS_USLEEP:       ret = sys_usleep((unsigned int)arg1); break;
        case SYS_GETTIMEOFDAY: ret = sys_gettimeofday((void*)arg1, (void*)arg2); break;
        case SYS_SOCKET:       ret = sys_socket((int)arg1, (int)arg2, (int)arg3); break;
        case SYS_CONNECT:      ret = sys_connect((int)arg1, (const neo_sockaddr_in*)arg2, (int)arg3); break;
        case SYS_BIND:         ret = sys_bind((int)arg1, (const neo_sockaddr_in*)arg2, (int)arg3); break;
        case SYS_LISTEN:       ret = sys_listen((int)arg1, (int)arg2); break;
        case SYS_ACCEPT:       ret = sys_accept((int)arg1, (neo_sockaddr_in*)arg2, (int*)arg3); break;
        case SYS_SEND:         ret = sys_send((int)arg1, (const void*)arg2, (int)arg3, (int)arg4); break;
        case SYS_RECV:         ret = sys_recv((int)arg1, (void*)arg2, (int)arg3, (int)arg4); break;
        case SYS_SENDTO:       ret = sys_sendto((int)arg1, (const void*)arg2, (int)arg3, (int)arg4,
                                                (const neo_sockaddr_in*)arg5,
                                                (int)sizeof(neo_sockaddr_in)); break;
        case SYS_RECVFROM:     ret = sys_recvfrom((int)arg1, (void*)arg2, (int)arg3, (int)arg4,
                                                  (neo_sockaddr_in*)arg5,
                                                  (int*)sizeof(neo_sockaddr_in)); break;
        case SYS_SHUTDOWN:     ret = sys_shutdown((int)arg1, (int)arg2); break;
        case SYS_POWEROFF:     power_off(); break;
        case SYS_REBOOT:       power_reboot(); break;
        case SYS_OPENPTY: {
            int m = -1, s = -1;
            int r = pty_create(&m, &s);
            if (r < 0) { ret = (uint32_t)-ENOMEM; break; }
            int* amaster = (int*)(uintptr_t)arg1;
            int* aslave = (int*)(uintptr_t)arg2;
            if (amaster) *amaster = m;
            if (aslave) *aslave = s;
            ret = 0;
            break;
        }
        case SYS_TCGETATTR:    ret = 0; break;
        case SYS_TCSETATTR:    ret = 0; break;
        case SYS_SELECT:       ret = sys_select((int)arg1, (uint32_t*)arg2, (uint32_t*)arg3,
                                                (uint32_t*)arg4, arg5); break;
        case SYS_POLL:         ret = sys_poll((pollfd_t*)arg1, (int)arg2, arg3); break;
        default:               ret = (uint32_t)-ENOSYS; break;
    }
    regs[7] = ret;
}

static int sys_write(int fd, const char* buf, int len) {
    if (len < 0) return -EINVAL;
    if (fd >= 100) return -EBADF;
    if (fd >= PIPE_FD_BASE && fd < PIPE_FD_END) return pipe_write_fd(fd, buf, len);
    if (fd >= 3) return fat32_write(fd, buf, len);
    for (int i = 0; i < len; i++) serial_putchar(buf[i]);
    return len;
}

static int sys_read(int fd, char* buf, int len) {
    if (fd < 0 || fd >= 100) return -EBADF;
    if (fd >= PIPE_FD_BASE && fd < PIPE_FD_END) return pipe_read_fd(fd, buf, len);
    if (fd >= 3) return fat32_read(fd, buf, len);
    return -EBADF;
}

static void sys_exit(int code) {
    (void)code;
    process_t* cur = process_get_current();
    if (cur) {
        cur->state = PROCESS_TERMINATED;
        process_ready_remove(cur);
    }
    schedule();
}

static void sys_yield(void) {
    schedule();
}

static uint32_t next_user_vaddr = 0x08000000;

static uint32_t sys_alloc(int nbytes) {
    uint32_t pages = (uint32_t)(nbytes + 4095) / 4096;
    if (pages == 0) pages = 1;
    uint32_t base = next_user_vaddr;
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t phys = pmm_alloc_page();
        if (!phys) return 0;
        if (!map_page(next_user_vaddr, phys, PAGE_PRESENT | PAGE_WRITE | PAGE_USER)) {
            pmm_free_page(phys);
            return 0;
        }
        next_user_vaddr += 4096;
    }
    return base;
}

static int sys_draw_frame(uint8_t* buf) {
    uint8_t* front = (uint8_t*)gfx_get_fb();
    if (!front || !fb_width || !fb_height) return -1;
    uint32_t row_bytes = fb_width * 4;
    for (uint32_t y = 0; y < fb_height; y++) {
        uint8_t* d = front + y * fb_pitch;
        uint8_t* s = buf + y * row_bytes;
        for (uint32_t i = 0; i < row_bytes; i++) d[i] = s[i];
    }
    return 0;
}

static int sys_play_audio(const void* buf, int len) {
    (void)buf; (void)len;
    return -ENOSYS;
}

static int sys_fork(void) {
    return process_fork_current();
}

static int sys_exec(const char* path) {
    (void)path;
    return -ENOSYS;
}

static int sys_open(const char* path, int flags) {
    int fd = fat32_open(path, flags);
    if (fd < 0) return -ENOENT;
    return fd;
}

static int sys_close(int fd) {
    if (fd >= 100) return sys_socket_close(fd);
    if (fd >= PIPE_FD_BASE && fd < PIPE_FD_END) return pipe_close(fd);
    if (fd < 0) return -EBADF;
    fat32_close(fd);
    return 0;
}

static int sys_getpid(void) {
    process_t* cur = process_get_current();
    return cur ? cur->pid : -1;
}

static int sys_sleep(unsigned int seconds) {
    uint32_t start = timer_ticks;
    uint32_t target = seconds * 100;
    while (timer_ticks - start < target)
        schedule();
    return 0;
}

static int sys_usleep(unsigned int usec) {
    uint32_t start = timer_ticks;
    uint32_t target = (usec / 10000);
    if (target == 0 && usec > 0) target = 1;
    while (timer_ticks - start < target)
        schedule();
    return 0;
}

typedef struct { int tv_sec; int tv_usec; } timeval_t;

static int sys_gettimeofday(void* tv, void* tz) {
    (void)tz;
    if (!tv) return -EINVAL;
    timeval_t* t = (timeval_t*)tv;
    t->tv_sec = (int)(timer_ticks / 100);
    t->tv_usec = (int)((timer_ticks % 100) * 10000);
    return 0;
}

void syscall_init(void) {
    socket_init();
}
