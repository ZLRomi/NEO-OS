#ifndef SIGNAL_H
#define SIGNAL_H

#include <stdint.h>

#define SIGINT   2
#define SIGKILL  9
#define SIGSEGV  11
#define SIGPIPE  13
#define SIGTERM  15
#define SIGCHLD  17
#define NSIG     32

typedef void (*sighandler_t)(int);

#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

typedef struct {
    sighandler_t handler;
    uint32_t mask;
    uint32_t flags;
} sigaction_t;

void signal_init(void);
int signal_send(int pid, int sig);
sighandler_t signal_set_handler(int sig, sighandler_t handler);
int signal_sigaction(int sig, const sigaction_t* act, sigaction_t* oldact);
int signal_check_pending(void);
void signal_deliver(int sig);
int signal_procmask(int how, const uint32_t* set, uint32_t* oldset);

int  sys_kill(int pid, int sig);
int  sys_signal(int sig, uint32_t handler);
int  sys_sigaction(int sig, const void* act, void* oldact);
int  sys_sigprocmask(int how, const uint32_t* set, uint32_t* oldset);

#endif
