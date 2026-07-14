#ifndef RSA_H
#define RSA_H

#include <stdint.h>

int rsa_encrypt(const uint8_t* mod, int mod_len,
                const uint8_t* exp, int exp_len,
                const uint8_t* in, int in_len,
                uint8_t* out, int out_max);

#endif
