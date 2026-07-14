#include "signal.h"
#include "process.h"
#include "sched.h"
#include "syscall.h"
#include "printk.h"
#include "klog.h"

static int valid_signal(int sig) {
    return sig > 0 && sig < NSIG;
}

void signal_init(void) {
    printk("signal: subsystem ready (NSIG=%d)\n", NSIG);
}

int signal_send(int pid, int sig) {
    if (!valid_signal(sig)) return -EINVAL;
    if (pid <= 0) return -EINVAL;
    process_t* p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_TERMINATED) return -ESRCH;
    p->pending_signals |= (1u << sig);
    klog_write_level(KLOG_INFO, "signal: send sig=%d to pid=%d\n", sig, pid);
    return 0;
}

sighandler_t signal_set_handler(int sig, sighandler_t handler) {
    if (!valid_signal(sig)) return (sighandler_t)(long)-EINVAL;
    if (sig == SIGKILL) return (sighandler_t)(long)-EINVAL;
    process_t* cur = process_get_current();
    if (!cur) return (sighandler_t)(long)-ESRCH;
    sighandler_t old = (sighandler_t)(uintptr_t)cur->signal_handlers[sig];
    cur->signal_handlers[sig] = (uint32_t)(uintptr_t)handler;
    return old;
}

int signal_sigaction(int sig, const sigaction_t* act, sigaction_t* oldact) {
    if (!valid_signal(sig)) return -EINVAL;
    if (sig == SIGKILL) return -EINVAL;
    process_t* cur = process_get_current();
    if (!cur) return -ESRCH;
    if (oldact) {
        oldact->handler = (sighandler_t)(uintptr_t)cur->signal_handlers[sig];
        oldact->mask = cur->signal_mask;
        oldact->flags = 0;
    }
    if (act) {
        cur->signal_handlers[sig] = (uint32_t)(uintptr_t)act->handler;
    }
    return 0;
}

int signal_check_pending(void) {
    process_t* cur = process_get_current();
    if (!cur) return 0;
    uint32_t pending = cur->pending_signals & ~cur->signal_mask;
    if (pending == 0) return 0;
    for (int i = 1; i < NSIG; i++) {
        if (pending & (1u << i)) return i;
    }
    return 0;
}

void signal_deliver(int sig) {
    process_t* cur = process_get_current();
    if (!cur) return;
    cur->pending_signals &= ~(1u << sig);
    uint32_t h = cur->signal_handlers[sig];
    if (h == (uint32_t)(uintptr_t)SIG_IGN) return;
    if (h == (uint32_t)(uintptr_t)SIG_DFL) {
        // SIGCHLD 默认忽略
        if (sig == SIGCHLD) return;
        printk("signal: default terminate pid=%d sig=%d\n", cur->pid, sig);
        klog_write_level(KLOG_WARN, "signal: default terminate pid=%d sig=%d\n", cur->pid, sig);
        cur->state = PROCESS_TERMINATED;
        process_ready_remove(cur);
        schedule();
        return;
    }
    // 简化版：内核态直接调用注册的处理函数
    sighandler_t handler = (sighandler_t)(uintptr_t)h;
    klog_write_level(KLOG_INFO, "signal: deliver sig=%d handler=0x%x\n", sig, h);
    handler(sig);
}

int signal_procmask(int how, const uint32_t* set, uint32_t* oldset) {
    process_t* cur = process_get_current();
    if (!cur) return -ESRCH;
    if (oldset) *oldset = cur->signal_mask;
    if (set) {
        uint32_t m = *set & ~(1u << SIGKILL);
        switch (how) {
            case SIG_BLOCK:   cur->signal_mask |= m; break;
            case SIG_UNBLOCK: cur->signal_mask &= ~m; break;
            case SIG_SETMASK: cur->signal_mask = m; break;
            default: return -EINVAL;
        }
    }
    return 0;
}

int sys_kill(int pid, int sig) {
    return signal_send(pid, sig);
}

int sys_signal(int sig, uint32_t handler) {
    sighandler_t old = signal_set_handler(sig, (sighandler_t)(uintptr_t)handler);
    return (int)(long)old;
}

int sys_sigaction(int sig, const void* act, void* oldact) {
    return signal_sigaction(sig, (const sigaction_t*)act, (sigaction_t*)oldact);
}

int sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset) {
    return signal_procmask(how, set, oldset);
}
