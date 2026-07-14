#ifndef TTY_H
#define TTY_H

#include <stdint.h>

#define TTY_MAX 7
#define TTY_BUF_SIZE 256
#define PTY_MAX 8

typedef struct {
    char inbuf[TTY_BUF_SIZE];
    int in_head, in_tail;
    char outbuf[TTY_BUF_SIZE];
    int out_head, out_tail;
    int echo;
    int canonical;
    int foreground_pid;
    char line[TTY_BUF_SIZE];
    int line_len;
    int line_ready;
} tty_t;

typedef struct {
    char m2s[TTY_BUF_SIZE];
    int m2s_head, m2s_tail;
    char s2m[TTY_BUF_SIZE];
    int s2m_head, s2m_tail;
    int in_use;
} pty_t;

void tty_init(void);
int  tty_read(int minor, char* buf, int len);
int  tty_write(int minor, const char* buf, int len);
void tty_input(int minor, char c);
int  tty_switch(int minor);
int  tty_get_current(void);
tty_t* tty_get(int minor);

int pty_create(int* master, int* slave);
int pty_write_master(int fd, const char* buf, int len);
int pty_read_slave(int fd, char* buf, int len);
int pty_write_slave(int fd, const char* buf, int len);
int pty_read_master(int fd, char* buf, int len);

#endif
