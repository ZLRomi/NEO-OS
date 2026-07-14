#ifndef TLS_H
#define TLS_H

#include <stdint.h>

int  tls_connect(uint32_t ip, uint16_t port, const char* host, uint32_t timeout_ticks);
int  tls_send(int sid, const uint8_t* data, uint16_t len);
int  tls_recv(int sid, uint8_t* buf, uint16_t max_len, uint32_t timeout_ticks);
void tls_close(int sid);

#endif
