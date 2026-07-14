#ifndef X509_H
#define X509_H

#include <stdint.h>

int x509_parse_rsa_pubkey(const uint8_t* der, int der_len,
                          uint8_t* mod, int* mod_len, int mod_max,
                          uint8_t* exp, int* exp_len, int exp_max);

#endif
