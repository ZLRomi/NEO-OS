#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>

#define AF_INET       2
#define AF_INET6      10

#define SOCK_STREAM   1
#define SOCK_DGRAM    2

#define SOL_SOCKET    1

#define SOCKET_MAX    16

typedef struct {
    uint32_t s_addr;
} neo_in_addr;

typedef struct {
    uint16_t    sin_family;
    uint16_t    sin_port;
    neo_in_addr sin_addr;
    uint8_t     sin_zero[8];
} neo_sockaddr_in;

void socket_init(void);

int sys_socket(int domain, int type, int protocol);
int sys_connect(int fd, const neo_sockaddr_in* addr, int addrlen);
int sys_bind(int fd, const neo_sockaddr_in* addr, int addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, neo_sockaddr_in* addr, int* addrlen);
int sys_send(int fd, const void* buf, int len, int flags);
int sys_recv(int fd, void* buf, int len, int flags);
int sys_sendto(int fd, const void* buf, int len, int flags,
               const neo_sockaddr_in* addr, int addrlen);
int sys_recvfrom(int fd, void* buf, int len, int flags,
                 neo_sockaddr_in* addr, int* addrlen);
int sys_shutdown(int fd, int how);
int sys_socket_close(int fd);

#endif
