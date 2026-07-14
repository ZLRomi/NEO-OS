#ifndef AES_H
#define AES_H

#include <stdint.h>

#define AES_BLOCK_SIZE 16

void aes128_set_key(const uint8_t key[16], uint32_t rk[44]);
void aes128_encrypt_block(const uint32_t rk[44], uint8_t block[16]);
void aes128_decrypt_block(const uint32_t rk[44], uint8_t block[16]);

#endif
