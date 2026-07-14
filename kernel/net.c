#include "net.h"
#include "e1000.h"
#include "idt.h"
#include "printk.h"
#include "serial.h"
#include "rtc.h"
#include <stdint.h>

#define ETH_TYPE_ARP 0x0806
#define ETH_TYPE_IP  0x0800

#define IP_PROTO_ICMP 1
#define IP_PROTO_UDP  17
#define IP_PROTO_TCP  6

#define ARP_OPER_REQUEST 1
#define ARP_OPER_REPLY   2

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67
#define DNS_PORT         53
#define NTP_PORT         123

uint8_t  local_mac[6];
uint32_t local_ip = 0;
uint32_t gateway_ip = 0;
uint32_t dns_ip = 0;
uint32_t subnet_mask = 0;

struct eth_hdr {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_pkt {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} __attribute__((packed));

struct ip_hdr {
    uint8_t  ihl_version;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct udp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    uint16_t checksum;
} __attribute__((packed));

struct icmp_echo {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} __attribute__((packed));

struct dhcp_packet {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint32_t magic;
    uint8_t  options[312];
} __attribute__((packed));

static uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static uint16_t ntohs(uint16_t v) { return htons(v); }
static uint32_t htonl(uint32_t v) {
    return ((v >> 24) & 0xFF)
         | ((v >> 8)  & 0xFF00)
         | ((v << 8)  & 0xFF0000)
         | ((v << 24) & 0xFF000000);
}
static uint32_t ntohl(uint32_t v) { return htonl(v); }

static void mem_copy(void* d, const void* s, int n) {
    uint8_t* dst = (uint8_t*)d;
    const uint8_t* src = (const uint8_t*)s;
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

static void mem_set(void* d, int v, int n) {
    uint8_t* dst = (uint8_t*)d;
    for (int i = 0; i < n; i++) dst[i] = (uint8_t)v;
}

static uint16_t ip_checksum(const void* data, int len) {
    const uint16_t* p = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += ntohs(*p);
        p++;
        len -= 2;
    }
    if (len) sum += *((const uint8_t*)p) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void eth_fill(struct eth_hdr* eth, const uint8_t* dst, uint16_t type) {
    mem_copy(eth->dst, dst, 6);
    mem_copy(eth->src, local_mac, 6);
    eth->type = htons(type);
}

/* ===== ARP ===== */
#define ARP_CACHE_SIZE 16
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

static void arp_update(uint32_t ip, const uint8_t* mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mem_copy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].valid = 1;
            arp_cache[i].ip = ip;
            mem_copy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
}

static int arp_lookup(uint32_t ip, uint8_t* mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            mem_copy(mac_out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_send(uint16_t oper, const uint8_t* dst_mac,
                     uint32_t target_ip, uint32_t sender_ip) {
    uint8_t buf[64];
    struct eth_hdr* eth = (struct eth_hdr*)buf;
    struct arp_pkt* arp = (struct arp_pkt*)(buf + sizeof(struct eth_hdr));

    eth_fill(eth, dst_mac, ETH_TYPE_ARP);
    arp->htype = htons(1);
    arp->ptype = htons(ETH_TYPE_IP);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(oper);
    mem_copy(arp->sender_mac, local_mac, 6);
    arp->sender_ip = htonl(sender_ip);
    mem_copy(arp->target_mac, dst_mac, 6);
    arp->target_ip = htonl(target_ip);

    e1000_send(buf, sizeof(struct eth_hdr) + sizeof(struct arp_pkt));
}

static void arp_request(uint32_t ip) {
    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    arp_send(ARP_OPER_REQUEST, broadcast, ip, local_ip);
}

static void arp_handle(struct eth_hdr* eth, int len) {
    if (len < (int)(sizeof(struct eth_hdr) + sizeof(struct arp_pkt))) return;
    struct arp_pkt* arp = (struct arp_pkt*)((uint8_t*)eth + sizeof(struct eth_hdr));
    if (ntohs(arp->htype) != 1 || ntohs(arp->ptype) != ETH_TYPE_IP) return;

    uint32_t sender_ip = ntohl(arp->sender_ip);
    arp_update(sender_ip, arp->sender_mac);

    if (ntohs(arp->oper) == ARP_OPER_REQUEST) {
        uint32_t target_ip = ntohl(arp->target_ip);
        if (target_ip == local_ip) {
            arp_send(ARP_OPER_REPLY, arp->sender_mac, sender_ip, local_ip);
        }
    }
}

static int arp_resolve(uint32_t ip, uint8_t* mac_out, uint32_t timeout_ticks) {
    if (arp_lookup(ip, mac_out)) return 1;
    arp_request(ip);
    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        net_poll_one();
        if (arp_lookup(ip, mac_out)) return 1;
    }
    return 0;
}

/* ===== IP / UDP transmit ===== */
int net_send_ip(uint32_t dst_ip, uint8_t proto, const void* payload, uint16_t payload_len) {
    uint8_t buf[2048];
    if (payload_len > (uint16_t)(sizeof(buf) - sizeof(struct eth_hdr) - sizeof(struct ip_hdr))) return 0;

    uint8_t dst_mac[6];
    if (dst_ip == 0xFFFFFFFF) {
        uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        mem_copy(dst_mac, bcast, 6);
    } else {
        uint32_t next_ip = dst_ip;
        if (local_ip && subnet_mask) {
            if ((dst_ip & subnet_mask) != (local_ip & subnet_mask)) {
                next_ip = gateway_ip ? gateway_ip : dst_ip;
            }
        }
        if (!arp_resolve(next_ip, dst_mac, 150)) return 0;
    }

    struct eth_hdr* eth = (struct eth_hdr*)buf;
    eth_fill(eth, dst_mac, ETH_TYPE_IP);

    struct ip_hdr* ip = (struct ip_hdr*)(buf + sizeof(struct eth_hdr));
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + payload_len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->proto = proto;
    ip->checksum = 0;
    ip->src = htonl(local_ip);
    ip->dst = htonl(dst_ip);
    ip->checksum = htons(ip_checksum(ip, 20));

    mem_copy(buf + sizeof(struct eth_hdr) + sizeof(struct ip_hdr), payload, payload_len);
    return e1000_send(buf, sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + payload_len);
}

int net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void* data, uint16_t len) {
    uint8_t payload[1536];
    if (len + sizeof(struct udp_hdr) > sizeof(payload)) return 0;
    struct udp_hdr* udp = (struct udp_hdr*)payload;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->len = htons(sizeof(struct udp_hdr) + len);
    udp->checksum = 0;
    mem_copy(payload + sizeof(struct udp_hdr), data, len);
    return net_send_ip(dst_ip, IP_PROTO_UDP, payload, sizeof(struct udp_hdr) + len);
}

/* ===== ICMP ===== */
static volatile int ping_done = 0;
static uint16_t ping_id = 0;
static uint16_t ping_seq = 0;

int ping(uint32_t dst_ip, uint32_t timeout_ticks) {
    uint8_t payload[64];
    struct icmp_echo* icmp = (struct icmp_echo*)payload;
    icmp->type = 8;
    icmp->code = 0;
    icmp->checksum = 0;
    ping_id = (uint16_t)(timer_ticks & 0xFFFF);
    ping_seq = 1;
    icmp->id = htons(ping_id);
    icmp->seq = htons(ping_seq);
    const char* data = "NEO-OS ping";
    int data_len = 11;
    mem_copy(payload + sizeof(struct icmp_echo), data, data_len);
    int total = sizeof(struct icmp_echo) + data_len;
    icmp->checksum = htons(ip_checksum(payload, total));

    ping_done = 0;
    if (!net_send_ip(dst_ip, IP_PROTO_ICMP, payload, total)) return 0;

    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        net_poll_one();
        if (ping_done) return 1;
    }
    return 0;
}

static void icmp_handle(struct ip_hdr* ip, int ip_len) {
    int icmp_len = ip_len - 20;
    if (icmp_len < (int)sizeof(struct icmp_echo)) return;
    struct icmp_echo* icmp = (struct icmp_echo*)((uint8_t*)ip + 20);
    if (icmp->type == 0 && ntohs(icmp->id) == ping_id && ntohs(icmp->seq) == ping_seq) {
        ping_done = 1;
    }
}

/* ===== DHCP ===== */
static volatile int dhcp_done = 0;
static uint32_t dhcp_xid = 0x12345678;
static uint32_t dhcp_offered = 0;
static uint32_t dhcp_server_id = 0;

static void dhcp_build_options(uint8_t* opts, int* len, uint8_t msg_type,
                               uint32_t requested_ip, uint32_t server_id) {
    int p = 0;
    opts[p++] = 53; opts[p++] = 1; opts[p++] = msg_type;
    if (requested_ip) {
        opts[p++] = 50; opts[p++] = 4;
        opts[p++] = (requested_ip >> 24) & 0xFF;
        opts[p++] = (requested_ip >> 16) & 0xFF;
        opts[p++] = (requested_ip >> 8)  & 0xFF;
        opts[p++] =  requested_ip        & 0xFF;
    }
    if (server_id) {
        opts[p++] = 54; opts[p++] = 4;
        opts[p++] = (server_id >> 24) & 0xFF;
        opts[p++] = (server_id >> 16) & 0xFF;
        opts[p++] = (server_id >> 8)  & 0xFF;
        opts[p++] =  server_id        & 0xFF;
    }
    opts[p++] = 55; opts[p++] = 3;
    opts[p++] = 1;   // subnet mask
    opts[p++] = 3;   // router
    opts[p++] = 6;   // DNS
    opts[p++] = 255; // end
    *len = p;
}

static void dhcp_send(uint8_t msg_type, uint32_t requested_ip, uint32_t server_id) {
    uint8_t buf[1024];
    mem_set(buf, 0, sizeof(buf));
    struct eth_hdr* eth = (struct eth_hdr*)buf;
    uint8_t bcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_fill(eth, bcast, ETH_TYPE_IP);

    struct ip_hdr* ip = (struct ip_hdr*)(buf + sizeof(struct eth_hdr));
    ip->ihl_version = 0x45;
    ip->tos = 0;
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 128;
    ip->proto = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src = 0;
    ip->dst = 0xFFFFFFFF;

    struct udp_hdr* udp = (struct udp_hdr*)(buf + sizeof(struct eth_hdr) + sizeof(struct ip_hdr));
    udp->src_port = htons(DHCP_CLIENT_PORT);
    udp->dst_port = htons(DHCP_SERVER_PORT);
    udp->checksum = 0;

    struct dhcp_packet* dhcp = (struct dhcp_packet*)(buf + sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr));
    dhcp->op = 1;
    dhcp->htype = 1;
    dhcp->hlen = 6;
    dhcp->hops = 0;
    dhcp->xid = htonl(dhcp_xid);
    dhcp->secs = 0;
    dhcp->flags = htons(0x8000); // broadcast
    dhcp->ciaddr = 0;
    dhcp->yiaddr = 0;
    dhcp->siaddr = 0;
    dhcp->giaddr = 0;
    mem_copy(dhcp->chaddr, local_mac, 6);
    dhcp->magic = htonl(0x63825363);

    int opt_len = 0;
    dhcp_build_options(dhcp->options, &opt_len, msg_type, requested_ip, server_id);

    int dhcp_len = sizeof(struct dhcp_packet) - sizeof(dhcp->options) + opt_len;
    udp->len = htons(sizeof(struct udp_hdr) + dhcp_len);
    ip->total_len = htons(20 + sizeof(struct udp_hdr) + dhcp_len);
    ip->checksum = htons(ip_checksum(ip, 20));

    e1000_send(buf, sizeof(struct eth_hdr) + sizeof(struct ip_hdr) + sizeof(struct udp_hdr) + dhcp_len);
}

static int dhcp_get_option(uint8_t* opts, int len, uint8_t code, uint8_t* out, int out_len) {
    int i = 0;
    while (i < len) {
        uint8_t op = opts[i];
        if (op == 255) break;
        if (op == 0) { i++; continue; }
        if (i + 1 >= len) break;
        uint8_t l = opts[i + 1];
        if (i + 2 + l > len) break;
        if (op == code) {
            int copy = l < out_len ? l : out_len;
            for (int j = 0; j < copy; j++) out[j] = opts[i + 2 + j];
            return copy;
        }
        i += 2 + l;
    }
    return 0;
}

static void dhcp_handle(struct ip_hdr* ip, int ip_len) {
    int udp_offset = 20;
    if (ip_len < udp_offset + (int)sizeof(struct udp_hdr)) return;
    struct udp_hdr* udp = (struct udp_hdr*)((uint8_t*)ip + udp_offset);
    if (ntohs(udp->dst_port) != DHCP_CLIENT_PORT) return;

    int dhcp_offset = udp_offset + sizeof(struct udp_hdr);
    int dhcp_len = ip_len - dhcp_offset;
    if (dhcp_len < (int)(sizeof(struct dhcp_packet) - 312)) return;
    struct dhcp_packet* dhcp = (struct dhcp_packet*)((uint8_t*)ip + dhcp_offset);
    if (ntohl(dhcp->xid) != dhcp_xid) return;
    if (dhcp->magic != htonl(0x63825363)) return;

    uint8_t msg_type = 0;
    if (!dhcp_get_option(dhcp->options, dhcp_len - ((int)sizeof(struct dhcp_packet) - 312), 53, &msg_type, 1)) return;

    if (msg_type == 2) { // offer
        dhcp_offered = ntohl(dhcp->yiaddr);
        uint8_t sid[4];
        if (dhcp_get_option(dhcp->options, dhcp_len - ((int)sizeof(struct dhcp_packet) - 312), 54, sid, 4)) {
            dhcp_server_id = ((uint32_t)sid[0] << 24) | ((uint32_t)sid[1] << 16) |
                             ((uint32_t)sid[2] << 8)  |  (uint32_t)sid[3];
        }
        dhcp_send(3, dhcp_offered, dhcp_server_id); // request
    } else if (msg_type == 5) { // ack
        local_ip = ntohl(dhcp->yiaddr);
        uint8_t mask[4], router[4], dns[4];
        if (dhcp_get_option(dhcp->options, dhcp_len - ((int)sizeof(struct dhcp_packet) - 312), 1, mask, 4)) {
            subnet_mask = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) |
                          ((uint32_t)mask[2] << 8)  |  (uint32_t)mask[3];
        }
        if (dhcp_get_option(dhcp->options, dhcp_len - ((int)sizeof(struct dhcp_packet) - 312), 3, router, 4)) {
            gateway_ip = ((uint32_t)router[0] << 24) | ((uint32_t)router[1] << 16) |
                         ((uint32_t)router[2] << 8)  |  (uint32_t)router[3];
        }
        if (dhcp_get_option(dhcp->options, dhcp_len - ((int)sizeof(struct dhcp_packet) - 312), 6, dns, 4)) {
            dns_ip = ((uint32_t)dns[0] << 24) | ((uint32_t)dns[1] << 16) |
                     ((uint32_t)dns[2] << 8)  |  (uint32_t)dns[3];
        }
        dhcp_done = 1;
    }
}

/* ===== DNS ===== */
struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t questions;
    uint16_t answers;
    uint16_t authority;
    uint16_t additional;
} __attribute__((packed));

static volatile int dns_done = 0;
static uint16_t dns_query_id = 0;
static uint32_t dns_result = 0;

static int dns_encode_name(uint8_t* out, const char* name) {
    int p = 0;
    while (*name) {
        const char* start = name;
        while (*name && *name != '.') name++;
        int len = name - start;
        out[p++] = (uint8_t)len;
        for (int i = 0; i < len; i++) out[p++] = start[i];
        if (*name == '.') name++;
    }
    out[p++] = 0;
    return p;
}

uint32_t net_dns_resolve(const char* name, uint32_t timeout_ticks) {
    if (!dns_ip) return 0;

    static char cache_name[128];
    static uint32_t cache_ip = 0;
    if (cache_ip) {
        int same = 1;
        for (int i = 0; name[i] || cache_name[i]; i++) {
            if (name[i] != cache_name[i]) { same = 0; break; }
        }
        if (same) return cache_ip;
    }
    uint8_t query[512];
    int p = 0;
    dns_query_id = (uint16_t)(timer_ticks & 0xFFFF);
    struct dns_header* hdr = (struct dns_header*)query;
    hdr->id = htons(dns_query_id);
    hdr->flags = htons(0x0100);
    hdr->questions = htons(1);
    hdr->answers = 0;
    hdr->authority = 0;
    hdr->additional = 0;
    p = sizeof(struct dns_header);
    p += dns_encode_name(query + p, name);
    query[p++] = 0x00; query[p++] = 0x01; // A
    query[p++] = 0x00; query[p++] = 0x01; // IN

    dns_done = 0;
    dns_result = 0;
    uint16_t src_port = (uint16_t)(40000 + (timer_ticks % 1000));
    serial_write("DNS: query ");
    serial_write(name);
    serial_write(" to ");
    char ipb[16];
    net_ip_to_str(dns_ip, ipb);
    serial_write(ipb);
    serial_write(" sp=");
    char spb[8]; int spn = 0;
    int spv = src_port;
    if (spv >= 10000) spb[spn++] = '0' + spv / 10000;
    if (spv >= 1000)  spb[spn++] = '0' + (spv / 1000) % 10;
    if (spv >= 100)   spb[spn++] = '0' + (spv / 100) % 10;
    if (spv >= 10)    spb[spn++] = '0' + (spv / 10) % 10;
    spb[spn++] = '0' + spv % 10;
    spb[spn] = '\0';
    serial_write(spb);
    serial_write("\n");
    if (!net_udp_send(dns_ip, src_port, DNS_PORT, query, p)) {
        serial_write("DNS: send failed\n");
        return 0;
    }

    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        net_poll_one();
        if (dns_done) {
            serial_write("DNS: done\n");
            if (dns_result) {
                int i = 0;
                while (name[i] && i < 127) { cache_name[i] = name[i]; i++; }
                cache_name[i] = '\0';
                cache_ip = dns_result;
            }
            return dns_result;
        }
    }
    serial_write("DNS: timeout\n");
    return 0;
}

static void dns_handle(struct ip_hdr* ip, int ip_len) {
    int udp_offset = 20;
    if (ip_len < udp_offset + (int)sizeof(struct udp_hdr)) return;
    struct udp_hdr* udp = (struct udp_hdr*)((uint8_t*)ip + udp_offset);
    if (ntohs(udp->src_port) != DNS_PORT) return;
    int dns_offset = udp_offset + sizeof(struct udp_hdr);
    int dns_len = ip_len - dns_offset;
    if (dns_len < (int)sizeof(struct dns_header)) return;
    struct dns_header* hdr = (struct dns_header*)((uint8_t*)ip + dns_offset);
    if (ntohs(hdr->id) != dns_query_id) return;
    if ((ntohs(hdr->flags) & 0x8000) == 0) return;

    uint8_t* p = (uint8_t*)hdr + sizeof(struct dns_header);
    uint16_t qcount = ntohs(hdr->questions);
    uint16_t acount = ntohs(hdr->answers);
    int rem = dns_len - sizeof(struct dns_header);

    for (int i = 0; i < qcount; i++) {
        while (rem > 0 && *p != 0) {
            if ((*p & 0xC0) == 0xC0) { p += 2; rem -= 2; break; }
            int l = *p;
            p += l + 1; rem -= l + 1;
        }
        if (*p == 0) { p++; rem--; }
        p += 4; rem -= 4;
    }

    for (int i = 0; i < acount && rem > 0; i++) {
        if ((*p & 0xC0) == 0xC0) { p += 2; rem -= 2; }
        else { while (rem > 0 && *p != 0) { int l = *p; p += l + 1; rem -= l + 1; } if (rem > 0) { p++; rem--; } }
        if (rem < 10) break;
        uint16_t type = (p[0] << 8) | p[1];
        uint16_t cls  = (p[2] << 8) | p[3];
        uint16_t rdlen = (p[8] << 8) | p[9];
        p += 10; rem -= 10;
        if (type == 1 && cls == 1 && rdlen == 4) {
            dns_result = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                         ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
            dns_done = 1;
            return;
        }
        p += rdlen; rem -= rdlen;
    }
}

/* ===== NTP ===== */
struct ntp_packet {
    uint8_t  li_vn_mode;
    uint8_t  stratum;
    uint8_t  poll;
    uint8_t  precision;
    uint32_t root_delay;
    uint32_t root_dispersion;
    uint32_t reference_id;
    uint32_t ref_ts_sec;
    uint32_t ref_ts_frac;
    uint32_t orig_ts_sec;
    uint32_t orig_ts_frac;
    uint32_t rx_ts_sec;
    uint32_t rx_ts_frac;
    uint32_t tx_ts_sec;
    uint32_t tx_ts_frac;
} __attribute__((packed));

static volatile int ntp_done = 0;
static volatile uint32_t ntp_tx_sec = 0;

static void unix_to_datetime(uint32_t unix_sec, int* year, int* month, int* day,
                             int* hour, int* minute, int* second) {
    int y = 1970;
    int m = 1;
    int d = 1;
    uint32_t days = unix_sec / 86400;
    uint32_t rem = unix_sec % 86400;

    while (1) {
        int leap = ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0));
        int year_days = leap ? 366 : 365;
        if (days < (uint32_t)year_days) break;
        days -= year_days;
        y++;
    }

    static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    while (1) {
        int leap = ((y % 4 == 0) && (y % 100 != 0 || y % 400 == 0));
        int dim = mdays[m - 1];
        if (m == 2 && leap) dim = 29;
        if (days < (uint32_t)dim) break;
        days -= dim;
        m++;
    }
    d += (int)days;

    *hour = (int)(rem / 3600);
    rem %= 3600;
    *minute = (int)(rem / 60);
    *second = (int)(rem % 60);
    *year = y;
    *month = m;
    *day = d;
}

static void ntp_handle(struct ip_hdr* ip, int ip_len) {
    int udp_offset = 20;
    if (ip_len < udp_offset + (int)sizeof(struct udp_hdr)) return;
    struct udp_hdr* udp = (struct udp_hdr*)((uint8_t*)ip + udp_offset);
    if (ntohs(udp->src_port) != NTP_PORT) return;
    int ntp_offset = udp_offset + sizeof(struct udp_hdr);
    int ntp_len = ip_len - ntp_offset;
    if (ntp_len < (int)sizeof(struct ntp_packet)) return;
    struct ntp_packet* pkt = (struct ntp_packet*)((uint8_t*)ip + ntp_offset);
    uint8_t mode = pkt->li_vn_mode & 0x07;
    uint8_t stratum = pkt->stratum;
    if (mode != 4 || stratum == 0) return;
    ntp_tx_sec = ntohl(pkt->tx_ts_sec);
    ntp_done = 1;
}

int ntp_sync_time(uint32_t timeout_ticks) {
    if (!local_ip || !gateway_ip) return 0;
    struct ntp_packet pkt;
    mem_set(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = 0x1B; // li=0, vn=3, mode=3 (client)

    uint32_t ntp_server = net_dns_resolve("pool.ntp.org", 200);
    if (!ntp_server) ntp_server = net_dns_resolve("time.windows.com", 200);
    if (!ntp_server) ntp_server = net_dns_resolve("time.google.com", 200);
    if (!ntp_server) {
        /* try Cloudflare NTP anycast */
        ntp_server = net_parse_ip("162.159.200.1");
    }
    if (!ntp_server) return 0;

    uint16_t src_port = (uint16_t)(42000 + (timer_ticks % 1000));
    ntp_done = 0;
    ntp_tx_sec = 0;
    if (!net_udp_send(ntp_server, src_port, NTP_PORT, &pkt, sizeof(pkt))) {
        serial_write("NTP: send failed\n");
        return 0;
    }

    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        net_poll_one();
        if (ntp_done && ntp_tx_sec > 0) {
            uint32_t unix_sec = ntp_tx_sec - 2208988800u;
            int y, m, d, h, min, s;
            unix_to_datetime(unix_sec, &y, &m, &d, &h, &min, &s);
            rtc_time_t t;
            t.year = y; t.month = m; t.day = d;
            t.hour = h; t.minute = min; t.second = s;
            rtc_set_time(&t);
            serial_write("NTP: time synced ");
            char buf[32];
            int n = 0;
            buf[n++] = '0' + (y / 1000) % 10;
            buf[n++] = '0' + (y / 100) % 10;
            buf[n++] = '0' + (y / 10) % 10;
            buf[n++] = '0' + y % 10;
            buf[n++] = '-';
            buf[n++] = '0' + (m / 10) % 10;
            buf[n++] = '0' + m % 10;
            buf[n++] = '-';
            buf[n++] = '0' + (d / 10) % 10;
            buf[n++] = '0' + d % 10;
            buf[n++] = ' ';
            buf[n++] = '0' + (h / 10) % 10;
            buf[n++] = '0' + h % 10;
            buf[n++] = ':';
            buf[n++] = '0' + (min / 10) % 10;
            buf[n++] = '0' + min % 10;
            buf[n++] = ':';
            buf[n++] = '0' + (s / 10) % 10;
            buf[n++] = '0' + s % 10;
            buf[n] = '\0';
            serial_write(buf);
            serial_write("\n");
            return 1;
        }
    }
    serial_write("NTP: timeout\n");
    return 0;
}

/* ===== TCP ===== */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2
#define TCP_FIN_WAIT1   3
#define TCP_FIN_WAIT2   4
#define TCP_CLOSE_WAIT  5
#define TCP_CLOSING     6
#define TCP_LAST_ACK    7
#define TCP_TIME_WAIT   8

#define MAX_TCP_CONNS 2

static tcp_conn_t tcp_conns[MAX_TCP_CONNS];

struct tcp_hdr {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t data_offset_flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed));

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void* tcp_data, int len) {
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += IP_PROTO_TCP;
    sum += (uint16_t)len;

    const uint16_t* p = (const uint16_t*)tcp_data;
    int l = len;
    while (l > 1) { sum += ntohs(*p); p++; l -= 2; }
    if (l) sum += *((const uint8_t*)p) << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static int tcp_send_packet(tcp_conn_t* conn, const void* data, uint16_t len, uint8_t flags,
                           const uint8_t* opts, int opt_len) {
    static uint8_t buf[2048];
    int hdr_len = 20 + opt_len;
    if (hdr_len % 4) hdr_len += 4 - (hdr_len % 4);
    if ((int)len + hdr_len > (int)sizeof(buf)) return 0;
    struct tcp_hdr* tcp = (struct tcp_hdr*)buf;
    tcp->src_port = htons(conn->src_port);
    tcp->dst_port = htons(conn->dst_port);
    tcp->seq = htonl(conn->seq);
    tcp->ack = htonl(conn->ack);
    tcp->data_offset_flags = htons(((hdr_len / 4) << 12) | flags);
    tcp->window = htons((uint16_t)(TCP_RX_BUF_SIZE - conn->rx_len));
    tcp->checksum = 0;
    tcp->urgent = 0;
    if (opt_len > 0) mem_copy(buf + sizeof(struct tcp_hdr), opts, opt_len);
    int pad = hdr_len - 20 - opt_len;
    for (int i = 0; i < pad; i++) buf[sizeof(struct tcp_hdr) + opt_len + i] = 0;
    mem_copy(buf + hdr_len, data, len);
    int total = hdr_len + len;
    tcp->checksum = htons(tcp_checksum(local_ip, conn->dst_ip, buf, total));
    return net_send_ip(conn->dst_ip, IP_PROTO_TCP, buf, total);
}

tcp_conn_t* tcp_find_conn(uint16_t src_port) {
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state != TCP_CLOSED && tcp_conns[i].src_port == src_port) {
            return &tcp_conns[i];
        }
    }
    return 0;
}

uint16_t tcp_connect(uint32_t dst_ip, uint16_t dst_port, uint32_t timeout_ticks) {
    tcp_conn_t* conn = 0;
    for (int i = 0; i < MAX_TCP_CONNS; i++) {
        if (tcp_conns[i].state == TCP_CLOSED) { conn = &tcp_conns[i]; break; }
    }
    if (!conn) return 0;

    mem_set(conn, 0, sizeof(*conn));
    conn->dst_ip = dst_ip;
    conn->dst_port = dst_port;
    conn->src_port = (uint16_t)(50000 + (timer_ticks % 1000));
    conn->seq = timer_ticks ? timer_ticks : 1;
    conn->ack = 0;
    conn->rx_next_seq = 0;
    conn->state = TCP_SYN_SENT;
    conn->rx_len = 0;
    conn->rx_fin = 0;

    uint8_t syn_opts[4] = {0x02, 0x04, 0x04, 0xB0}; // MSS = 1200
    tcp_send_packet(conn, 0, 0, TCP_FLAG_SYN, syn_opts, sizeof(syn_opts));

    uint32_t start = timer_ticks;
    while (conn->state != TCP_ESTABLISHED && timer_ticks - start < timeout_ticks) {
        net_poll_one();
    }
    return (conn->state == TCP_ESTABLISHED) ? conn->src_port : 0;
}

int tcp_send(uint16_t src_port, const void* data, uint16_t len) {
    tcp_conn_t* conn = tcp_find_conn(src_port);
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    if (!tcp_send_packet(conn, data, len, TCP_FLAG_PSH | TCP_FLAG_ACK, 0, 0)) return -1;
    conn->seq += len;
    return len;
}

int tcp_recv(uint16_t src_port, void* buf, uint16_t max_len, uint32_t timeout_ticks) {
    tcp_conn_t* conn = tcp_find_conn(src_port);
    if (!conn) return -1;
    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        net_poll_one();
        if (conn->rx_len > 0) {
            int copy = conn->rx_len < (int)max_len ? conn->rx_len : (int)max_len;
            mem_copy(buf, conn->rx_buf, copy);
            for (int i = copy; i < conn->rx_len; i++) conn->rx_buf[i - copy] = conn->rx_buf[i];
            conn->rx_len -= copy;
            return copy;
        }
        if (conn->state == TCP_CLOSED && conn->rx_len == 0) return 0;
        if (conn->rx_fin && conn->rx_len == 0) return 0;
    }
    return -1;
}

void tcp_close(uint16_t src_port) {
    tcp_conn_t* conn = tcp_find_conn(src_port);
    if (!conn) return;
    if (conn->state == TCP_ESTABLISHED || conn->state == TCP_CLOSE_WAIT) {
        tcp_send_packet(conn, 0, 0, TCP_FLAG_FIN | TCP_FLAG_ACK, 0, 0);
        conn->state = (conn->state == TCP_ESTABLISHED) ? TCP_FIN_WAIT1 : TCP_LAST_ACK;
        uint32_t start = timer_ticks;
        while (conn->state != TCP_CLOSED && timer_ticks - start < 100) {
            net_poll_one();
        }
    }
    conn->state = TCP_CLOSED;
}

static void tcp_handle(struct ip_hdr* ip, int ip_len) {
    (void)ip_len;
    int ip_hdr_len = (ip->ihl_version & 0x0F) * 4;
    int total_len = ntohs(ip->total_len);
    int tcp_len = total_len - ip_hdr_len;
    if (tcp_len < (int)sizeof(struct tcp_hdr)) return;
    struct tcp_hdr* tcp = (struct tcp_hdr*)((uint8_t*)ip + ip_hdr_len);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint16_t doff_flags = ntohs(tcp->data_offset_flags);
    int hdr_len = (doff_flags >> 12) * 4;
    uint8_t flags = doff_flags & 0x3F;
    uint32_t seq = ntohl(tcp->seq);
    uint32_t ack = ntohl(tcp->ack);

    tcp_conn_t* conn = tcp_find_conn(dst_port);
    if (!conn) return;

    if (conn->state == TCP_SYN_SENT && (flags & TCP_FLAG_SYN) && (flags & TCP_FLAG_ACK)) {
        if (ack == conn->seq + 1) {
            conn->seq++;
            conn->ack = seq + 1;
            conn->rx_next_seq = seq + 1;
            conn->state = TCP_ESTABLISHED;
            tcp_send_packet(conn, 0, 0, TCP_FLAG_ACK, 0, 0);
        }
        return;
    }

    if (conn->state != TCP_ESTABLISHED && conn->state != TCP_FIN_WAIT1 &&
        conn->state != TCP_FIN_WAIT2 && conn->state != TCP_LAST_ACK) return;

    int data_len = tcp_len - hdr_len;
    if (data_len > 0) {
        if (seq == conn->rx_next_seq) {
            if (conn->rx_len + data_len < TCP_RX_BUF_SIZE) {
                mem_copy(conn->rx_buf + conn->rx_len, (uint8_t*)tcp + hdr_len, data_len);
                conn->rx_len += data_len;
                conn->rx_next_seq += data_len;
            }
        }
        conn->ack = conn->rx_next_seq;
        tcp_send_packet(conn, 0, 0, TCP_FLAG_ACK, 0, 0);
    }

    if (flags & TCP_FLAG_FIN) {
        if (seq + data_len == conn->rx_next_seq) {
            conn->rx_next_seq++;
        }
        conn->ack = conn->rx_next_seq;
        tcp_send_packet(conn, 0, 0, TCP_FLAG_ACK, 0, 0);
        if (conn->state == TCP_ESTABLISHED || conn->state == TCP_FIN_WAIT1) {
            conn->state = TCP_CLOSE_WAIT;
            conn->rx_fin = 1;
            tcp_close(conn->src_port);
        } else {
            conn->state = TCP_CLOSED;
        }
    }
}

/* ===== packet dispatch ===== */
static void udp_dispatch(struct ip_hdr* ip, int ip_len) {
    int udp_offset = 20;
    struct udp_hdr* udp = (struct udp_hdr*)((uint8_t*)ip + udp_offset);
    uint16_t dst_port = ntohs(udp->dst_port);
    if (dst_port == DHCP_CLIENT_PORT) dhcp_handle(ip, ip_len);
    else if (dst_port >= 40000 && dst_port < 41000) dns_handle(ip, ip_len);
    else if (dst_port >= 42000 && dst_port < 43000) ntp_handle(ip, ip_len);
}

static void ip_handle(struct eth_hdr* eth, int len) {
    if (len < (int)(sizeof(struct eth_hdr) + 20)) return;
    struct ip_hdr* ip = (struct ip_hdr*)((uint8_t*)eth + sizeof(struct eth_hdr));
    int ip_len = (ip->ihl_version & 0x0F) * 4;
    if (ip_len < 20) return;
    int total_len = ntohs(ip->total_len);
    if (total_len > len - (int)sizeof(struct eth_hdr)) return;
    uint32_t dst = ntohl(ip->dst);
    if (local_ip && dst != local_ip && dst != 0xFFFFFFFF) return;

    if (ip->proto == IP_PROTO_ICMP) icmp_handle(ip, total_len);
    else if (ip->proto == IP_PROTO_UDP) udp_dispatch(ip, total_len);
    else if (ip->proto == IP_PROTO_TCP) tcp_handle(ip, total_len);
}

void net_poll_one(void) {
    static uint8_t buf[2048];
    int len = e1000_recv(buf, sizeof(buf));
    if (len <= 0) {
        __asm__ volatile("hlt");
        return;
    }
    if (len < (int)sizeof(struct eth_hdr)) return;
    struct eth_hdr* eth = (struct eth_hdr*)buf;
    uint16_t type = ntohs(eth->type);
    if (type == ETH_TYPE_ARP) arp_handle(eth, len);
    else if (type == ETH_TYPE_IP) ip_handle(eth, len);
}

/* ===== init ===== */
uint32_t net_parse_ip(const char* s) {
    uint32_t ip = 0;
    int part = 0;
    int dots = 0;
    while (*s) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            part = part * 10 + (c - '0');
        } else if (c == '.') {
            ip = (ip << 8) | (part & 0xFF);
            part = 0;
            dots++;
        } else break;
        s++;
    }
    ip = (ip << 8) | (part & 0xFF);
    return (dots == 3) ? ip : 0;
}

void net_ip_to_str(uint32_t ip, char* out) {
    int p[4] = { (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF };
    int n = 0;
    for (int i = 0; i < 4; i++) {
        int v = p[i];
        if (v >= 100) out[n++] = '0' + v / 100;
        if (v >= 10)  out[n++] = '0' + (v / 10) % 10;
        out[n++] = '0' + v % 10;
        if (i < 3) out[n++] = '.';
    }
    out[n] = '\0';
}

int net_init(void) {
    e1000_get_mac(local_mac);
    mem_set(arp_cache, 0, sizeof(arp_cache));

    dhcp_xid = timer_ticks ? timer_ticks : 0x12345678;
    dhcp_done = 0;
    dhcp_offered = 0;
    dhcp_server_id = 0;

    dhcp_send(1, 0, 0); // discover
    uint32_t start = timer_ticks;
    while (!dhcp_done && timer_ticks - start < 300) {
        net_poll_one();
    }

    if (!dhcp_done) {
        local_ip    = net_parse_ip("10.0.2.15");
        subnet_mask = net_parse_ip("255.255.255.0");
        gateway_ip  = net_parse_ip("10.0.2.2");
        dns_ip      = net_parse_ip("10.0.2.3");
        printk("DHCP failed, using static fallback\n");
        serial_write("NET: DHCP failed, static fallback\n");
        return 0;
    }

    if (!dns_ip) dns_ip = net_parse_ip("10.0.2.3");
    serial_write("NET: DHCP ok\n");
    printk("DHCP ok: IP=");
    char buf[16];
    net_ip_to_str(local_ip, buf);
    printk(buf);
    printk(" gw=");
    net_ip_to_str(gateway_ip, buf);
    printk(buf);
   printk(" dns=");
    net_ip_to_str(dns_ip, buf);
    printk(buf);
    printk(" hex=");
    printk("%x", dns_ip);
    printk("\n");
    return 1;}
