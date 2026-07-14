#include "tls.h"
#include "net.h"
#include "sha256.h"
#include "aes.h"
#include "rsa.h"
#include "x509.h"
#include "idt.h"
#include "serial.h"
#include <stdint.h>

#define TLS_RECORD_CHANGE_CIPHER_SPEC 20
#define TLS_RECORD_ALERT 21
#define TLS_RECORD_HANDSHAKE 22
#define TLS_RECORD_APPDATA 23

#define TLS_CIPHER_SUITE 0x003C

#define MAX_TLS_SESSIONS 2
#define TLS_HANDSHAKE_BUF 16384
#define TLS_RECORD_BUF 18432

#define TLS_STATE_FREE 0
#define TLS_STATE_HANDSHAKE 1
#define TLS_STATE_READY 2
#define TLS_STATE_CLOSED 3

typedef struct {
    int used;
    int state;
    uint16_t tcp_port;

    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t pre_master_secret[48];
    uint8_t master_secret[48];

    uint8_t client_mac[32];
    uint8_t server_mac[32];
    uint8_t client_key[16];
    uint8_t server_key[16];
    uint32_t client_rk[44];
    uint32_t server_rk[44];

    uint8_t server_mod[256];
    int server_mod_len;
    uint8_t server_exp[256];
    int server_exp_len;

    uint64_t write_seq;
    uint64_t read_seq;
    int write_encrypted;
    int read_encrypted;

    uint8_t handshake_msgs[TLS_HANDSHAKE_BUF];
    int handshake_len;
    int handshake_processed;
    int server_hello_done;
} tls_session_t;

static tls_session_t tls_sessions[MAX_TLS_SESSIONS];

static uint8_t tls_send_buf[TLS_RECORD_BUF];
static uint8_t tls_recv_buf[TLS_RECORD_BUF];
static uint8_t tls_mac_buf[TLS_RECORD_BUF];
static uint8_t tls_plain_buf[TLS_RECORD_BUF];
static uint8_t tls_handshake_data[16384];

static void mem_set(void* d, int v, int n) {
    uint8_t* dst = (uint8_t*)d;
    for (int i = 0; i < n; i++) dst[i] = (uint8_t)v;
}

static uint16_t get_be16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t get_be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static void set_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void set_be24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static void set_be64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - i * 8));
}

static uint32_t rdtsc32(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi;
}

static void tls_random_fill(uint8_t* buf, int len) {
    static uint32_t seed = 0;
    for (int i = 0; i < len; i++) {
        uint32_t v = (uint32_t)timer_ticks + rdtsc32() + (uint32_t)i + seed;
        seed = v * 1103515245U + 12345U;
        buf[i] = (uint8_t)(v ^ (v >> 8) ^ (v >> 16) ^ (v >> 24));
    }
}

static void tls_prf(const uint8_t* secret, int secret_len,
                    const char* label,
                    const uint8_t* seed, int seed_len,
                    uint8_t* out, int out_len) {
    uint8_t ls[128];
    int ls_len = 0;
    while (label[ls_len] && ls_len < (int)sizeof(ls) - seed_len - 1) {
        ls[ls_len] = (uint8_t)label[ls_len];
        ls_len++;
    }
    for (int i = 0; i < seed_len && ls_len < (int)sizeof(ls); i++) {
        ls[ls_len++] = seed[i];
    }

    uint8_t a[32];
    hmac_sha256(secret, secret_len, ls, ls_len, a);

    int offset = 0;
    while (offset < out_len) {
        uint8_t h[32];
        uint8_t msg[32 + 128];
        for (int i = 0; i < 32; i++) msg[i] = a[i];
        for (int i = 0; i < ls_len; i++) msg[32 + i] = ls[i];
        hmac_sha256(secret, secret_len, msg, 32 + ls_len, h);
        int copy = out_len - offset;
        if (copy > 32) copy = 32;
        for (int i = 0; i < copy; i++) out[offset++] = h[i];
        hmac_sha256(secret, secret_len, a, 32, a);
    }
}

static int tls_tcp_read_exact(tls_session_t* s, uint8_t* buf, int len, uint32_t timeout) {
    int got = 0;
    uint32_t start = timer_ticks;
    uint32_t last_data = timer_ticks;
    while (got < len) {
        if (timer_ticks - start >= timeout * 4) return -1;
        if (timer_ticks - last_data >= timeout) return -1;
        int n = tcp_recv(s->tcp_port, buf + got, (uint16_t)(len - got), 50);
        if (n > 0) {
            got += n;
            last_data = timer_ticks;
        } else if (n == 0) {
            return -1;
        }
    }
    return 0;
}

static int tls_record_send(tls_session_t* s, uint8_t type, const uint8_t* data, int len) {
    if (len < 0 || len > 16384) return -1;

    if (!s->write_encrypted) {
        if (5 + len > TLS_RECORD_BUF) return -1;
        tls_send_buf[0] = type;
        tls_send_buf[1] = 0x03;
        tls_send_buf[2] = 0x03;
        set_be16(tls_send_buf + 3, (uint16_t)len);
        for (int i = 0; i < len; i++) tls_send_buf[5 + i] = data[i];
        return (tcp_send(s->tcp_port, tls_send_buf, (uint16_t)(5 + len)) == 5 + len) ? 0 : -1;
    }

    // MAC input = seq(8) + type(1) + version(2) + len(2) + fragment
    int mac_input_len = 13 + len;
    if (mac_input_len > TLS_RECORD_BUF) return -1;
    set_be64(tls_mac_buf, s->write_seq);
    tls_mac_buf[8] = type;
    tls_mac_buf[9] = 0x03;
    tls_mac_buf[10] = 0x03;
    set_be16(tls_mac_buf + 11, (uint16_t)len);
    for (int i = 0; i < len; i++) tls_mac_buf[13 + i] = data[i];

    uint8_t mac[32];
    hmac_sha256(s->client_mac, 32, tls_mac_buf, mac_input_len, mac);

    int plain_len = len + 32;
    int pad = 16 - (plain_len % 16);
    if (pad == 0) pad = 16;
    int padded_len = plain_len + pad;
    if (padded_len > TLS_RECORD_BUF) return -1;

    for (int i = 0; i < len; i++) tls_plain_buf[i] = data[i];
    for (int i = 0; i < 32; i++) tls_plain_buf[len + i] = mac[i];
    for (int i = 0; i < pad; i++) tls_plain_buf[plain_len + i] = (uint8_t)(pad - 1);

    uint8_t iv[16];
    tls_random_fill(iv, 16);

    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = iv[i];
    for (int b = 0; b < padded_len / 16; b++) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = tls_plain_buf[b * 16 + i] ^ prev[i];
        aes128_encrypt_block(s->client_rk, block);
        for (int i = 0; i < 16; i++) {
            tls_plain_buf[b * 16 + i] = block[i];
            prev[i] = block[i];
        }
    }

    int rec_len = 5 + 16 + padded_len;
    if (rec_len > TLS_RECORD_BUF) return -1;
    tls_send_buf[0] = type;
    tls_send_buf[1] = 0x03;
    tls_send_buf[2] = 0x03;
    set_be16(tls_send_buf + 3, (uint16_t)(16 + padded_len));
    for (int i = 0; i < 16; i++) tls_send_buf[5 + i] = iv[i];
    for (int i = 0; i < padded_len; i++) tls_send_buf[5 + 16 + i] = tls_plain_buf[i];

    if (tcp_send(s->tcp_port, tls_send_buf, (uint16_t)rec_len) != rec_len) return -1;
    s->write_seq++;
    return 0;
}

static int tls_record_recv(tls_session_t* s, uint8_t* type_out,
                           uint8_t* data, int max_len, uint32_t timeout) {
    uint8_t hdr[5];
    if (tls_tcp_read_exact(s, hdr, 5, timeout) < 0) {
        serial_write("TLS: recv header failed\n");
        return -1;
    }

    uint8_t type = hdr[0];
    int payload_len = get_be16(hdr + 3);
    serial_write("TLS: recv header type=");
    {
        char h[3];
        h[0] = "0123456789ABCDEF"[type >> 4];
        h[1] = "0123456789ABCDEF"[type & 0xF];
        h[2] = '\0';
        serial_write(h);
    }
    serial_write(" len=");
    {
        char tbuf[8];
        int v = payload_len, ti = 0;
        if (v == 0) tbuf[ti++] = '0';
        while (v > 0) { tbuf[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) serial_putchar(tbuf[--ti]);
    }
    serial_write("\n");
    if (payload_len > TLS_RECORD_BUF) {
        serial_write("TLS: recv payload too large\n");
        return -1;
    }
    if (tls_tcp_read_exact(s, tls_recv_buf, payload_len, timeout) < 0) {
        serial_write("TLS: recv payload failed\n");
        return -1;
    }

    if (!s->read_encrypted) {
        *type_out = type;
        int n = payload_len;
        if (n > max_len) n = max_len;
        for (int i = 0; i < n; i++) data[i] = tls_recv_buf[i];
        return n;
    }

    if (payload_len < 32 || (payload_len - 16) % 16 != 0) {
        serial_write("TLS: recv encrypted len invalid\n");
        return -1;
    }
    int cipher_len = payload_len - 16;

    uint8_t prev[16];
    for (int i = 0; i < 16; i++) prev[i] = tls_recv_buf[i];

    for (int b = 0; b < cipher_len / 16; b++) {
        uint8_t block[16];
        for (int i = 0; i < 16; i++) block[i] = tls_recv_buf[16 + b * 16 + i];
        aes128_decrypt_block(s->server_rk, block);
        uint8_t next_prev[16];
        for (int i = 0; i < 16; i++) next_prev[i] = tls_recv_buf[16 + b * 16 + i];
        for (int i = 0; i < 16; i++) {
            tls_recv_buf[b * 16 + i] = block[i] ^ prev[i];
            prev[i] = next_prev[i];
        }
    }

    int plain_len = cipher_len;
    int pad = tls_recv_buf[plain_len - 1];
    if (pad > 16) {
        serial_write("TLS: recv pad invalid\n");
        return -1;
    }
    if (plain_len < pad + 1) {
        serial_write("TLS: recv plain too short\n");
        return -1;
    }
    plain_len -= (pad + 1);

    if (plain_len < 32) {
        serial_write("TLS: recv plain no mac\n");
        return -1;
    }
    int frag_len = plain_len - 32;
    uint8_t* frag = tls_recv_buf;
    uint8_t* mac = tls_recv_buf + frag_len;

    int mac_input_len = 13 + frag_len;
    if (mac_input_len > TLS_RECORD_BUF) {
        serial_write("TLS: recv mac input too large\n");
        return -1;
    }
    set_be64(tls_mac_buf, s->read_seq);
    tls_mac_buf[8] = type;
    tls_mac_buf[9] = 0x03;
    tls_mac_buf[10] = 0x03;
    set_be16(tls_mac_buf + 11, (uint16_t)frag_len);
    for (int i = 0; i < frag_len; i++) tls_mac_buf[13 + i] = frag[i];

    uint8_t expected[32];
    hmac_sha256(s->server_mac, 32, tls_mac_buf, mac_input_len, expected);
    for (int i = 0; i < 32; i++) if (mac[i] != expected[i]) {
        serial_write("TLS: recv mac mismatch\n");
        return -1;
    }

    s->read_seq++;
    *type_out = type;
    if (frag_len > max_len) frag_len = max_len;
    for (int i = 0; i < frag_len; i++) data[i] = frag[i];
    return frag_len;
}

static int tls_add_handshake(tls_session_t* s, const uint8_t* msg, int len) {
    if (s->handshake_len + len > TLS_HANDSHAKE_BUF) return -1;
    for (int i = 0; i < len; i++) s->handshake_msgs[s->handshake_len++] = msg[i];
    return 0;
}

static int build_client_hello(tls_session_t* s, uint8_t* out, int max, const char* host) {
    int host_len = 0;
    while (host && host[host_len]) host_len++;
    if (host_len > 255) host_len = 255;

    // signature_algorithms extension (required by many TLS 1.2 servers)
    // ext = 2(type) + 2(ext_len) + 2(list_len) + N*(hash,sig)
    int sigalgs_count = 4;
    int sigalgs_ext_len = 2 + 2 * sigalgs_count; // list_len + pairs
    int sni_ext_len = host_len ? (2 + 2 + 2 + 1 + 2 + host_len) : 0; // type + extlen + listlen + nametype + hostlen + host
    int ext_len = (host_len ? sni_ext_len : 0) + (4 + sigalgs_ext_len);

    // body = 2(ver) + 32(random) + 1(sid_len) + 2(cs_len) + 2(cs) + 1(cm_len) + 1(cm) + 2(ext_len) + ext
    int body_len = 43 + ext_len;
    int total = 4 + body_len;
    if (max < total) return -1;

    tls_random_fill(s->client_random, 32);
    int p = 0;
    out[p++] = 0x01;
    set_be24(out + p, body_len);
    p += 3;
    out[p++] = 0x03;
    out[p++] = 0x03;
    for (int i = 0; i < 32; i++) out[p++] = s->client_random[i];
    out[p++] = 0x00;
    out[p++] = 0x00; // cipher_suites length high
    out[p++] = 0x02; // cipher_suites length low
    out[p++] = (uint8_t)(TLS_CIPHER_SUITE >> 8);
    out[p++] = (uint8_t)(TLS_CIPHER_SUITE & 0xFF);
    out[p++] = 0x01;
    out[p++] = 0x00;
    set_be16(out + p, (uint16_t)ext_len);
    p += 2;

    if (host_len) {
        set_be16(out + p, 0x0000); // server_name
        p += 2;
        set_be16(out + p, (uint16_t)(2 + 1 + 2 + host_len));
        p += 2;
        set_be16(out + p, (uint16_t)(1 + 2 + host_len));
        p += 2;
        out[p++] = 0x00; // host_name
        set_be16(out + p, (uint16_t)host_len);
        p += 2;
        for (int i = 0; i < host_len; i++) out[p++] = host[i];
    }

    // signature_algorithms
    set_be16(out + p, 0x000D); // signature_algorithms
    p += 2;
    set_be16(out + p, (uint16_t)sigalgs_ext_len);
    p += 2;
    set_be16(out + p, (uint16_t)(2 * sigalgs_count));
    p += 2;
    out[p++] = 0x04; out[p++] = 0x01; // sha256 + rsa
    out[p++] = 0x02; out[p++] = 0x01; // sha1 + rsa
    out[p++] = 0x04; out[p++] = 0x03; // sha256 + ecdsa
    out[p++] = 0x02; out[p++] = 0x03; // sha1 + ecdsa

    return p;
}

static int parse_server_hello(tls_session_t* s, const uint8_t* body, int len) {
    if (len < 35) return -1;
    for (int i = 0; i < 32; i++) s->server_random[i] = body[2 + i];
    int sid_len = body[34];
    int pos = 35 + sid_len;
    if (pos + 3 > len) return -1;
    uint16_t cipher = get_be16(body + pos);
    if (cipher != TLS_CIPHER_SUITE) return -1;
    return 0;
}

static int parse_certificate(tls_session_t* s, const uint8_t* body, int len) {
    if (len < 3) return -1;
    int certs_len = get_be24(body);
    if (certs_len + 3 > len) return -1;
    const uint8_t* cp = body + 3;
    int clen = certs_len;
    if (clen < 3) return -1;
    int cert1_len = get_be24(cp);
    cp += 3;
    clen -= 3;
    if (cert1_len > clen) return -1;

    uint8_t mod[256];
    uint8_t exp[256];
    int mod_len = 0, exp_len = 0;
    if (x509_parse_rsa_pubkey(cp, cert1_len, mod, &mod_len, sizeof(mod),
                              exp, &exp_len, sizeof(exp)) < 0) {
        return -1;
    }
    if (mod_len > (int)sizeof(s->server_mod) || exp_len > (int)sizeof(s->server_exp)) return -1;
    for (int i = 0; i < mod_len; i++) s->server_mod[i] = mod[i];
    s->server_mod_len = mod_len;
    for (int i = 0; i < exp_len; i++) s->server_exp[i] = exp[i];
    s->server_exp_len = exp_len;
    return 0;
}

static int tls_process_handshake(tls_session_t* s, const uint8_t* data, int len) {
    if (tls_add_handshake(s, data, len) < 0) return -1;

    while (s->handshake_processed + 4 <= s->handshake_len) {
        uint8_t* p = s->handshake_msgs + s->handshake_processed;
        uint8_t type = p[0];
        int msg_len = (int)get_be24(p + 1);
        if (s->handshake_processed + 4 + msg_len > s->handshake_len) break;
        const uint8_t* body = p + 4;

        if (type == 2) {
            if (parse_server_hello(s, body, msg_len) < 0) return -1;
        } else if (type == 11) {
            if (parse_certificate(s, body, msg_len) < 0) return -1;
        } else if (type == 14) {
            s->server_hello_done = 1;
        }
        s->handshake_processed += 4 + msg_len;
    }
    return 0;
}

static void log_hex(const char* prefix, const uint8_t* data, int len);

static int send_client_key_exchange(tls_session_t* s) {
    s->pre_master_secret[0] = 0x03;
    s->pre_master_secret[1] = 0x03;
    tls_random_fill(s->pre_master_secret + 2, 46);

    int k = s->server_mod_len;
    if (k < 48 + 11) return -1;

    uint8_t em[256];
    em[0] = 0x00;
    em[1] = 0x02;
    for (int i = 2; i < k - 49; i++) {
        uint8_t rb;
        do { rb = (uint8_t)(rdtsc32() + i); } while (rb == 0);
        em[i] = rb;
    }
    em[k - 49] = 0x00;
    for (int i = 0; i < 48; i++) em[k - 48 + i] = s->pre_master_secret[i];

    log_hex("TLS: srv_mod=", s->server_mod, s->server_mod_len);
    log_hex("TLS: srv_exp=", s->server_exp, s->server_exp_len);
    uint8_t enc[256];
    log_hex("TLS: em=", em, k);
    serial_write("TLS: calling rsa_encrypt\n");
    if (rsa_encrypt(s->server_mod, s->server_mod_len,
                    s->server_exp, s->server_exp_len,
                    em, k, enc, k) != k) {
        serial_write("TLS: rsa_encrypt failed\n");
        return -1;
    }
    serial_write("TLS: rsa_encrypt done\n");
    log_hex("TLS: rsa_out=", enc, k);

    uint8_t msg[262];
    msg[0] = 0x10;
    set_be24(msg + 1, (uint32_t)(k + 2));
    set_be16(msg + 4, (uint16_t)k);
    for (int i = 0; i < k; i++) msg[6 + i] = enc[i];
    int msg_len = 6 + k;

    if (tls_record_send(s, TLS_RECORD_HANDSHAKE, msg, msg_len) < 0) return -1;
    if (tls_add_handshake(s, msg, msg_len) < 0) return -1;
    return 0;
}

static void log_hex(const char* prefix, const uint8_t* data, int len) {
    serial_write(prefix);
    for (int i = 0; i < len; i++) {
        serial_putchar("0123456789ABCDEF"[data[i] >> 4]);
        serial_putchar("0123456789ABCDEF"[data[i] & 0xF]);
    }
    serial_write("\n");
}

static int derive_keys(tls_session_t* s) {
    uint8_t seed[64];
    for (int i = 0; i < 32; i++) seed[i] = s->client_random[i];
    for (int i = 0; i < 32; i++) seed[32 + i] = s->server_random[i];
    tls_prf(s->pre_master_secret, 48, "master secret", seed, 64, s->master_secret, 48);

    log_hex("TLS: client_random=", s->client_random, 32);
    log_hex("TLS: server_random=", s->server_random, 32);
    log_hex("TLS: pre_master_sec=", s->pre_master_secret, 48);
    log_hex("TLS: master_secret=", s->master_secret, 48);

    for (int i = 0; i < 32; i++) seed[i] = s->server_random[i];
    for (int i = 0; i < 32; i++) seed[32 + i] = s->client_random[i];
    uint8_t key_block[96];
    tls_prf(s->master_secret, 48, "key expansion", seed, 64, key_block, 96);

    log_hex("TLS: key_block=", key_block, 96);

    int p = 0;
    for (int i = 0; i < 32; i++) s->client_mac[i] = key_block[p++];
    for (int i = 0; i < 32; i++) s->server_mac[i] = key_block[p++];
    for (int i = 0; i < 16; i++) s->client_key[i] = key_block[p++];
    for (int i = 0; i < 16; i++) s->server_key[i] = key_block[p++];

    aes128_set_key(s->client_key, s->client_rk);
    aes128_set_key(s->server_key, s->server_rk);
    return 0;
}

static int send_change_cipher_spec(tls_session_t* s) {
    uint8_t ccs[6] = {0x14, 0x03, 0x03, 0x00, 0x01, 0x01};
    if (tcp_send(s->tcp_port, ccs, 6) != 6) return -1;
    s->write_encrypted = 1;
    return 0;
}

static int send_finished(tls_session_t* s) {
    uint8_t hash[32];
    sha256(s->handshake_msgs, s->handshake_len, hash);
    uint8_t verify[12];
    tls_prf(s->master_secret, 48, "client finished", hash, 32, verify, 12);
    uint8_t msg[16];
    msg[0] = 0x14;
    set_be24(msg + 1, 12);
    for (int i = 0; i < 12; i++) msg[4 + i] = verify[i];

    serial_write("TLS: handshake_len=");
    {
        char tbuf[8]; int v = s->handshake_len, ti = 0;
        if (v == 0) tbuf[ti++] = '0';
        while (v > 0) { tbuf[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) serial_putchar(tbuf[--ti]);
    }
    serial_write(" hash=");
    for (int i = 0; i < 32; i++) {
        serial_putchar("0123456789ABCDEF"[hash[i] >> 4]);
        serial_putchar("0123456789ABCDEF"[hash[i] & 0xF]);
    }
    serial_write(" verify=");
    for (int i = 0; i < 12; i++) {
        serial_putchar("0123456789ABCDEF"[verify[i] >> 4]);
        serial_putchar("0123456789ABCDEF"[verify[i] & 0xF]);
    }
    serial_write("\n");

    serial_write("TLS: hs_data=");
    {
        int max_print = s->handshake_len;
        if (max_print > 300) max_print = 300;
        for (int i = 0; i < max_print; i++) {
            serial_putchar("0123456789ABCDEF"[s->handshake_msgs[i] >> 4]);
            serial_putchar("0123456789ABCDEF"[s->handshake_msgs[i] & 0xF]);
        }
        serial_write("\n");
    }
    serial_write("TLS: full_hs_data=");
    for (int i = 0; i < s->handshake_len; i++) {
        serial_putchar("0123456789ABCDEF"[s->handshake_msgs[i] >> 4]);
        serial_putchar("0123456789ABCDEF"[s->handshake_msgs[i] & 0xF]);
    }
    serial_write("\n");

    if (tls_add_handshake(s, msg, 16) < 0) return -1;
    return tls_record_send(s, TLS_RECORD_HANDSHAKE, msg, 16);
}

static int recv_change_cipher_spec(tls_session_t* s, uint32_t timeout) {
    uint8_t type, data[8];
    int n = tls_record_recv(s, &type, data, sizeof(data), timeout);
    if (n < 0) return -1;
    if (type == TLS_RECORD_ALERT) {
        serial_write("TLS: alert before CCS level=");
        if (n > 0) serial_putchar("0123456789ABCDEF"[data[0] >> 4]);
        if (n > 0) serial_putchar("0123456789ABCDEF"[data[0] & 0xF]);
        serial_write(" desc=");
        if (n > 1) serial_putchar("0123456789ABCDEF"[data[1] >> 4]);
        if (n > 1) serial_putchar("0123456789ABCDEF"[data[1] & 0xF]);
        serial_write("\n");
        return -1;
    }
    if (type != TLS_RECORD_CHANGE_CIPHER_SPEC) return -1;
    if (n != 1 || data[0] != 0x01) return -1;
    s->read_encrypted = 1;
    return 0;
}

static int recv_server_finished(tls_session_t* s, uint32_t timeout) {
    uint8_t type, data[32];
    int n = tls_record_recv(s, &type, data, sizeof(data), timeout);
    if (n < 0 || type != TLS_RECORD_HANDSHAKE) return -1;
    if (n < 4 + 12 || data[0] != 0x14) return -1;
    int msg_len = (int)get_be24(data + 1);
    if (msg_len != 12 || 4 + msg_len > n) return -1;

    uint8_t hash[32];
    sha256(s->handshake_msgs, s->handshake_len, hash);
    uint8_t expected[12];
    tls_prf(s->master_secret, 48, "server finished", hash, 32, expected, 12);
    for (int i = 0; i < 12; i++) if (data[4 + i] != expected[i]) return -1;
    return 0;
}

int tls_connect(uint32_t ip, uint16_t port, const char* host, uint32_t timeout_ticks) {
    serial_write("TLS: connect start host=");
    if (host) serial_write(host);
    serial_write("\n");
    int sid = -1;
    for (int i = 0; i < MAX_TLS_SESSIONS; i++) {
        if (!tls_sessions[i].used) { sid = i; break; }
    }
    if (sid < 0) { serial_write("TLS: no session slot\n"); return 0; }

    tls_session_t* s = &tls_sessions[sid];
    mem_set(s, 0, sizeof(*s));
    s->used = 1;
    s->state = TLS_STATE_HANDSHAKE;

    uint16_t port_tcp = tcp_connect(ip, port, timeout_ticks);
    if (!port_tcp) { serial_write("TLS: tcp connect failed\n"); s->used = 0; return 0; }
    s->tcp_port = port_tcp;
    serial_write("TLS: tcp connected\n");

    uint8_t ch[256];
    int ch_len = build_client_hello(s, ch, sizeof(ch), host);
    if (ch_len < 0) { serial_write("TLS: build CH failed\n"); tls_close(sid + 1); return 0; }
    if (tls_record_send(s, TLS_RECORD_HANDSHAKE, ch, ch_len) < 0) { serial_write("TLS: send CH failed\n"); tls_close(sid + 1); return 0; }
    if (tls_add_handshake(s, ch, ch_len) < 0) { serial_write("TLS: add CH failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: ClientHello sent len=");
    {
        char lenbuf[8];
        int lv = ch_len, li = 0;
        if (lv == 0) lenbuf[li++] = '0';
        while (lv > 0) { lenbuf[li++] = '0' + (lv % 10); lv /= 10; }
        while (li > 0) serial_putchar(lenbuf[--li]);
    }
    serial_write("\n");

    uint32_t start = timer_ticks;
    while (!s->server_hello_done) {
        if (timer_ticks - start >= timeout_ticks) { serial_write("TLS: handshake timeout\n"); tls_close(sid + 1); return 0; }
        uint8_t type;
        int n = tls_record_recv(s, &type, tls_handshake_data, sizeof(tls_handshake_data), 200);
        if (n < 0) continue;
        if (type == TLS_RECORD_ALERT) {
            serial_write("TLS: alert during handshake data=");
            for (int ai = 0; ai < n && ai < 2; ai++) {
                uint8_t v = tls_handshake_data[ai];
                char h[3];
                h[0] = "0123456789ABCDEF"[v >> 4];
                h[1] = "0123456789ABCDEF"[v & 0xF];
                h[2] = '\0';
                serial_write(h);
            }
            serial_write("\n");
            tls_close(sid + 1); return 0;
        }
        if (type != TLS_RECORD_HANDSHAKE) continue;
        if (tls_process_handshake(s, tls_handshake_data, n) < 0) { serial_write("TLS: process handshake failed\n"); tls_close(sid + 1); return 0; }
    }
    serial_write("TLS: ServerHelloDone received\n");

    if (s->server_mod_len == 0) { serial_write("TLS: no server cert\n"); tls_close(sid + 1); return 0; }
    {
        serial_write("TLS: server cert mod_len=");
        char tbuf[8]; int ti = 0, v = s->server_mod_len;
        if (v == 0) tbuf[ti++] = '0';
        while (v > 0) { tbuf[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) serial_putchar(tbuf[--ti]);
        serial_write(" exp_len=");
        ti = 0; v = s->server_exp_len;
        if (v == 0) tbuf[ti++] = '0';
        while (v > 0) { tbuf[ti++] = '0' + (v % 10); v /= 10; }
        while (ti > 0) serial_putchar(tbuf[--ti]);
        serial_write("\n");
    }

    uint32_t t0 = timer_ticks;
    if (send_client_key_exchange(s) < 0) { serial_write("TLS: CKE failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: ClientKeyExchange sent\n");
    if (derive_keys(s) < 0) { serial_write("TLS: derive keys failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: keys derived\n");
    if (send_change_cipher_spec(s) < 0) { serial_write("TLS: CCS send failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: CCS sent\n");
    if (send_finished(s) < 0) { serial_write("TLS: Finished send failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: Finished sent\n");
    {
        char tbuf[8];
        uint32_t dt = timer_ticks - t0;
        int ti = 0;
        if (dt == 0) tbuf[ti++] = '0';
        while (dt > 0) { tbuf[ti++] = '0' + (dt % 10); dt /= 10; }
        serial_write("TLS: send phase ticks=");
        while (ti > 0) serial_putchar(tbuf[--ti]);
        serial_write("\n");
    }

    if (recv_change_cipher_spec(s, timeout_ticks) < 0) { serial_write("TLS: CCS recv failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: CCS received\n");
    if (recv_server_finished(s, timeout_ticks) < 0) { serial_write("TLS: Finished recv failed\n"); tls_close(sid + 1); return 0; }
    serial_write("TLS: Finished received\n");

    s->state = TLS_STATE_READY;
    serial_write("TLS: ready\n");
    return sid + 1;
}

int tls_send(int sid, const uint8_t* data, uint16_t len) {
    if (sid < 1 || sid > MAX_TLS_SESSIONS) return -1;
    tls_session_t* s = &tls_sessions[sid - 1];
    if (!s->used || s->state != TLS_STATE_READY) return -1;

    int sent = 0;
    while (sent < len) {
        int chunk = len - sent;
        if (chunk > 1024) chunk = 1024;
        if (tls_record_send(s, TLS_RECORD_APPDATA, data + sent, chunk) < 0) return -1;
        sent += chunk;
    }
    return sent;
}

int tls_recv(int sid, uint8_t* buf, uint16_t max_len, uint32_t timeout_ticks) {
    if (sid < 1 || sid > MAX_TLS_SESSIONS) return -1;
    tls_session_t* s = &tls_sessions[sid - 1];
    if (!s->used || s->state != TLS_STATE_READY) return -1;

    uint32_t start = timer_ticks;
    while (timer_ticks - start < timeout_ticks) {
        uint8_t type;
        int n = tls_record_recv(s, &type, tls_handshake_data, sizeof(tls_handshake_data), 200);
        if (n < 0) continue;
        if (type == TLS_RECORD_ALERT) return -1;
        if (type == TLS_RECORD_HANDSHAKE) continue;
        if (type == TLS_RECORD_APPDATA) {
            int copy = n;
            if (copy > max_len) copy = max_len;
            for (int i = 0; i < copy; i++) buf[i] = tls_handshake_data[i];
            return copy;
        }
    }
    return -1;
}

void tls_close(int sid) {
    if (sid < 1 || sid > MAX_TLS_SESSIONS) return;
    tls_session_t* s = &tls_sessions[sid - 1];
    if (!s->used) return;
    tcp_close(s->tcp_port);
    s->used = 0;
    s->state = TLS_STATE_CLOSED;
}
