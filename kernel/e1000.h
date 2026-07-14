#ifndef E1000_H
#define E1000_H

#include <stdint.h>

int e1000_init(void);
int e1000_send(const void* data, uint16_t len);
int e1000_has_packet(void);
int e1000_recv(void* buf, uint16_t max_len);
void e1000_get_mac(uint8_t* out);

#endif
