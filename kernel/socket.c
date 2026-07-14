#include "socket.h"
#include "net.h"
#include "syscall.h"
#include "serial.h"
#include <stdint.h>

typedef struct {
    int      in_use;
    int      type;
    uint32_t remote_ip;
    uint16_t remote_port;
    uint16_t local_port;
    int      connected;
    int      bound;
} neo_socket_t;

static neo_socket_t sockets[SOCKET_MAX];
static uint16_t next_ephemeral = 40000;

static int sock_alloc(int type) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (!sockets[i].in_use) {
            sockets[i].in_use = 1;
            sockets[i].type = type;
            sockets[i].remote_ip = 0;
            sockets[i].remote_port = 0;
            sockets[i].local_port = 0;
            sockets[i].connected = 0;
            sockets[i].bound = 0;
            return i + 100;
        }
    }
    return -ENOMEM;
}

static neo_socket_t* sock_get(int fd) {
    int idx = fd - 100;
    if (idx < 0 || idx >= SOCKET_MAX || !sockets[idx].in_use) return 0;
    return &sockets[idx];
}

void socket_init(void) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        sockets[i].in_use = 0;
    }
}

int sys_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;
    return sock_alloc(type);
}

int sys_connect(int fd, const neo_sockaddr_in* addr, int addrlen) {
    if (!addr || addrlen < (int)sizeof(neo_sockaddr_in)) return -EINVAL;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EPROTONOSUPPORT;

    uint32_t ip = addr->sin_addr.s_addr;
    uint16_t port = addr->sin_port;

    uint16_t src = tcp_connect(ip, port, 500);
    if (!src) return -ECONNREFUSED;

    s->local_port = src;
    s->remote_ip = ip;
    s->remote_port = port;
    s->connected = 1;
    return 0;
}

int sys_bind(int fd, const neo_sockaddr_in* addr, int addrlen) {
    if (!addr || addrlen < (int)sizeof(neo_sockaddr_in)) return -EINVAL;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    s->local_port = addr->sin_port ? addr->sin_port : next_ephemeral++;
    s->bound = 1;
    (void)s;
    return 0;
}

int sys_listen(int fd, int backlog) {
    (void)backlog;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    return -ENOSYS;
}

int sys_accept(int fd, neo_sockaddr_in* addr, int* addrlen) {
    (void)addr;
    (void)addrlen;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM) return -EOPNOTSUPP;
    return -ENOSYS;
}

int sys_send(int fd, const void* buf, int len, int flags) {
    (void)flags;
    if (!buf || len <= 0) return -EINVAL;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM || !s->connected) return -ENOTCONN;

    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1400) chunk = 1400;
        int n = tcp_send(s->local_port, (const uint8_t*)buf + sent, (uint16_t)chunk);
        if (n <= 0) return sent > 0 ? sent : -EIO;
        sent += n;
    }
    return sent;
}

int sys_recv(int fd, void* buf, int len, int flags) {
    (void)flags;
    if (!buf || len <= 0) return -EINVAL;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_STREAM || !s->connected) return -ENOTCONN;

    int n = tcp_recv(s->local_port, buf, (uint16_t)len, 500);
    if (n < 0) return -EIO;
    return n;
}

int sys_sendto(int fd, const void* buf, int len, int flags,
               const neo_sockaddr_in* addr, int addrlen) {
    (void)flags;
    if (!buf || len <= 0 || !addr || addrlen < (int)sizeof(neo_sockaddr_in)) return -EINVAL;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type != SOCK_DGRAM) return -EOPNOTSUPP;

    uint16_t src = s->bound ? s->local_port : (uint16_t)(next_ephemeral++);
    if (!net_udp_send(addr->sin_addr.s_addr, src, addr->sin_port, buf, (uint16_t)len))
        return -EIO;
    s->local_port = src;
    return len;
}

int sys_recvfrom(int fd, void* buf, int len, int flags,
                 neo_sockaddr_in* addr, int* addrlen) {
    (void)fd;
    (void)buf;
    (void)len;
    (void)flags;
    (void)addr;
    (void)addrlen;
    return -ENOSYS;
}

int sys_shutdown(int fd, int how) {
    (void)how;
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type == SOCK_STREAM && s->connected) {
        tcp_close(s->local_port);
        s->connected = 0;
    }
    return 0;
}

int sys_socket_close(int fd) {
    neo_socket_t* s = sock_get(fd);
    if (!s) return -EBADF;
    if (s->type == SOCK_STREAM && s->connected)
        tcp_close(s->local_port);
    s->in_use = 0;
    return 0;
}
