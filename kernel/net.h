#ifndef NET_H
#define NET_H

#include <stdint.h>

extern uint8_t  local_mac[6];
extern uint32_t local_ip;
extern uint32_t gateway_ip;
extern uint32_t dns_ip;
extern uint32_t subnet_mask;

int      net_init(void);
void     net_poll_one(void);
int      net_send_ip(uint32_t dst_ip, uint8_t proto, const void* payload, uint16_t payload_len);
int      ping(uint32_t dst_ip, uint32_t timeout_ticks);
uint32_t net_dns_resolve(const char* name, uint32_t timeout_ticks);
uint32_t net_parse_ip(const char* s);
void     net_ip_to_str(uint32_t ip, char* out);
int      net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                      const void* data, uint16_t len);

/* TCP client */
#define TCP_RX_BUF_SIZE 16384

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t dst_ip;
    uint32_t seq;
    uint32_t ack;
    uint32_t rx_next_seq;
    int      state;
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    int      rx_len;
    int      rx_fin;
} tcp_conn_t;

uint16_t tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ticks);
int      tcp_send(uint16_t src_port, const void* data, uint16_t len);
int      tcp_recv(uint16_t src_port, void* buf, uint16_t max_len, uint32_t timeout_ticks);
void     tcp_close(uint16_t src_port);
tcp_conn_t* tcp_find_conn(uint16_t src_port);

int      ntp_sync_time(uint32_t timeout_ticks);

#endif
