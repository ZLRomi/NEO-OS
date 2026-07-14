#include "tty.h"
#include "signal.h"
#include "serial.h"
#include "terminal.h"
#include "printk.h"
#include <stdint.h>

static tty_t ttys[TTY_MAX];
static int current_tty = 0;
static pty_t ptys[PTY_MAX];

static void ring_put(char* buf, int size, int* tail, int head, char c) {
    int next = (*tail + 1) % size;
    if (next == head) return;
    buf[*tail] = c;
    *tail = next;
}

static int ring_get(char* buf, int size, int* head, int tail) {
    if (*head == tail) return -1;
    char c = buf[*head];
    *head = (*head + 1) % size;
    return (uint8_t)c;
}

static int ring_count(int head, int tail, int size) {
    int n = tail - head;
    if (n < 0) n += size;
    return n;
}

void tty_init(void) {
    int i;
    for (i = 0; i < TTY_MAX; i++) {
        ttys[i].in_head = 0;
        ttys[i].in_tail = 0;
        ttys[i].out_head = 0;
        ttys[i].out_tail = 0;
        ttys[i].echo = 1;
        ttys[i].canonical = 1;
        ttys[i].foreground_pid = 0;
        ttys[i].line_len = 0;
        ttys[i].line_ready = 0;
    }
    for (i = 0; i < PTY_MAX; i++) {
        ptys[i].in_use = 0;
        ptys[i].m2s_head = 0;
        ptys[i].m2s_tail = 0;
        ptys[i].s2m_head = 0;
        ptys[i].s2m_tail = 0;
    }
    current_tty = 0;
    printk("tty: %d virtual consoles initialized\n", TTY_MAX);
}

int tty_get_current(void) {
    return current_tty;
}

tty_t* tty_get(int minor) {
    if (minor < 0 || minor >= TTY_MAX) return 0;
    return &ttys[minor];
}

int tty_switch(int minor) {
    if (minor < 0 || minor >= TTY_MAX) return -1;
    if (minor == current_tty) return 0;
    current_tty = minor;
    printk("tty: switched to console %d\n", minor);
    return 0;
}

void tty_input(int minor, char c) {
    if (minor < 0 || minor >= TTY_MAX) return;
    tty_t* t = &ttys[minor];
    uint8_t uc = (uint8_t)c;

    if (c == 0x03) {
        if (t->foreground_pid > 0) {
            sys_kill(t->foreground_pid, SIGINT);
        }
        t->line_len = 0;
        if (t->echo) {
            tty_write(minor, "^C", 2);
            tty_write(minor, "\n", 1);
        }
        return;
    }

    if (c == '\b' || uc == 0x7F) {
        if (t->line_len > 0) {
            t->line_len--;
            if (t->echo) {
                tty_write(minor, "\b \b", 3);
            }
        }
        return;
    }

    if (c == '\n' || c == '\r') {
        if (t->line_len < TTY_BUF_SIZE - 1) {
            t->line[t->line_len++] = '\n';
        }
        for (int i = 0; i < t->line_len; i++) {
            ring_put(t->inbuf, TTY_BUF_SIZE, &t->in_tail, t->in_head, t->line[i]);
        }
        t->line_len = 0;
        t->line_ready = 1;
        if (t->echo) {
            tty_write(minor, "\n", 1);
        }
        return;
    }

    if (uc >= 0x20 && uc < 0x7F) {
        if (t->line_len < TTY_BUF_SIZE - 1) {
            t->line[t->line_len++] = c;
            if (t->echo) {
                tty_write(minor, &c, 1);
            }
        }
    }
}

int tty_read(int minor, char* buf, int len) {
    if (minor < 0 || minor >= TTY_MAX || !buf || len <= 0) return 0;
    tty_t* t = &ttys[minor];

    if (t->canonical && !t->line_ready) {
        return 0;
    }

    int n = 0;
    while (n < len) {
        int c = ring_get(t->inbuf, TTY_BUF_SIZE, &t->in_head, t->in_tail);
        if (c < 0) break;
        buf[n++] = (char)c;
        if (t->canonical && (char)c == '\n') break;
    }

    if (ring_count(t->in_head, t->in_tail, TTY_BUF_SIZE) == 0) {
        t->line_ready = 0;
    }

    return n;
}

int tty_write(int minor, const char* buf, int len) {
    if (minor < 0 || minor >= TTY_MAX || !buf) return 0;
    tty_t* t = &ttys[minor];
    int i;

    for (i = 0; i < len; i++) {
        ring_put(t->outbuf, TTY_BUF_SIZE, &t->out_tail, t->out_head, buf[i]);
        serial_putchar(buf[i]);
        terminal_putchar(buf[i]);
    }
    return len;
}

int pty_create(int* master, int* slave) {
    int i;
    for (i = 0; i < PTY_MAX; i++) {
        if (!ptys[i].in_use) {
            ptys[i].in_use = 1;
            ptys[i].m2s_head = 0;
            ptys[i].m2s_tail = 0;
            ptys[i].s2m_head = 0;
            ptys[i].s2m_tail = 0;
            if (master) *master = i;
            if (slave) *slave = i + 0x80;
            return 0;
        }
    }
    return -1;
}

static int pty_index(int fd) {
    if (fd >= 0 && fd < PTY_MAX) return fd;
    if (fd >= 0x80 && fd < 0x80 + PTY_MAX) return fd - 0x80;
    return -1;
}

int pty_write_master(int fd, const char* buf, int len) {
    int i = pty_index(fd);
    if (i < 0 || !buf) return -1;
    pty_t* p = &ptys[i];
    int n = 0;
    int k;
    for (k = 0; k < len; k++) {
        int next = (p->m2s_tail + 1) % TTY_BUF_SIZE;
        if (next == p->m2s_head) break;
        p->m2s[p->m2s_tail] = buf[k];
        p->m2s_tail = next;
        n++;
    }
    return n;
}

int pty_read_slave(int fd, char* buf, int len) {
    int i = pty_index(fd);
    if (i < 0 || !buf) return -1;
    pty_t* p = &ptys[i];
    int n = 0;
    int k;
    for (k = 0; k < len; k++) {
        if (p->m2s_head == p->m2s_tail) break;
        buf[k] = p->m2s[p->m2s_head];
        p->m2s_head = (p->m2s_head + 1) % TTY_BUF_SIZE;
        n++;
    }
    return n;
}

int pty_write_slave(int fd, const char* buf, int len) {
    int i = pty_index(fd);
    if (i < 0 || !buf) return -1;
    pty_t* p = &ptys[i];
    int n = 0;
    int k;
    for (k = 0; k < len; k++) {
        int next = (p->s2m_tail + 1) % TTY_BUF_SIZE;
        if (next == p->s2m_head) break;
        p->s2m[p->s2m_tail] = buf[k];
        p->s2m_tail = next;
        n++;
    }
    return n;
}

int pty_read_master(int fd, char* buf, int len) {
    int i = pty_index(fd);
    if (i < 0 || !buf) return -1;
    pty_t* p = &ptys[i];
    int n = 0;
    int k;
    for (k = 0; k < len; k++) {
        if (p->s2m_head == p->s2m_tail) break;
        buf[k] = p->s2m[p->s2m_head];
        p->s2m_head = (p->s2m_head + 1) % TTY_BUF_SIZE;
        n++;
    }
    return n;
}
