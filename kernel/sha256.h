#ifndef SHA256_H
#define SHA256_H

#include <stdint.h>
#include <stddef.h>

#define SHA256_BLOCK_SIZE 64
#define SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bit_len;
    uint8_t  buffer[64];
    uint32_t buffer_len;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t* ctx);
void sha256_update(sha256_ctx_t* ctx, const uint8_t* data, size_t len);
void sha256_final(sha256_ctx_t* ctx, uint8_t out[32]);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

void hmac_sha256(const uint8_t* key, size_t key_len,
                 const uint8_t* msg, size_t msg_len,
                 uint8_t out[32]);

#endif
