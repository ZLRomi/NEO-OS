#include "rsa.h"

#define BN_WORDS 128
#define BN_MUL_WORDS 256

typedef struct {
    uint16_t d[BN_WORDS];
} bn_t;

typedef struct {
    uint16_t d[BN_MUL_WORDS];
} bn_mul_t;

static void bn_zero(bn_t* a) {
    for (int i = 0; i < BN_WORDS; i++) a->d[i] = 0;
}

static void bn_cpy(bn_t* dst, const bn_t* src) {
    for (int i = 0; i < BN_WORDS; i++) dst->d[i] = src->d[i];
}

static void bn_from_bytes(bn_t* a, const uint8_t* data, int len) {
    bn_zero(a);
    int k = 0;
    for (int i = len - 1; i >= 0 && k < BN_WORDS * 2; i--, k++) {
        int word = k / 2;
        if (word >= BN_WORDS) break;
        if ((k & 1) == 0) {
            a->d[word] |= data[i];
        } else {
            a->d[word] |= ((uint16_t)data[i] << 8);
        }
    }
}

static void bn_to_bytes(const bn_t* a, uint8_t* out, int len) {
    for (int i = 0; i < len; i++) {
        int b = (len - 1 - i) * 8;
        int word = b / 16;
        int shift = ((b % 16) / 8) * 8;
        if (word < BN_WORDS) {
            out[i] = (uint8_t)(a->d[word] >> shift);
        } else {
            out[i] = 0;
        }
    }
}

static int bn_is_zero(const bn_t* a) {
    for (int i = 0; i < BN_WORDS; i++) if (a->d[i]) return 0;
    return 1;
}

static int bn_bitlen(const bn_t* a) {
    for (int i = BN_WORDS - 1; i >= 0; i--) {
        if (a->d[i]) {
            uint16_t v = a->d[i];
            int b = 0;
            while (v) { b++; v >>= 1; }
            return i * 16 + b;
        }
    }
    return 0;
}

static int __attribute__((unused)) bn_cmp(const bn_t* a, const bn_t* b) {
    for (int i = BN_WORDS - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) return (a->d[i] > b->d[i]) ? 1 : -1;
    }
    return 0;
}

static void __attribute__((unused)) bn_sub(bn_t* a, const bn_t* b) {
    int borrow = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        int sub = b->d[i] + borrow;
        int res = a->d[i] - sub;
        if (res < 0) { res += 65536; borrow = 1; } else { borrow = 0; }
        a->d[i] = (uint16_t)res;
    }
}

static void __attribute__((unused)) bn_shl1(bn_t* a) {
    uint16_t carry = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        uint16_t v = a->d[i];
        a->d[i] = (v << 1) | carry;
        carry = v >> 15;
    }
}

static void bn_shr1(bn_t* a) {
    uint16_t carry = 0;
    for (int i = BN_WORDS - 1; i >= 0; i--) {
        uint16_t v = a->d[i];
        a->d[i] = (v >> 1) | carry;
        carry = (v & 1) << 15;
    }
}

static void bn_mul(bn_mul_t* r, const bn_t* a, const bn_t* b) {
    uint64_t t[BN_MUL_WORDS];
    for (int i = 0; i < BN_MUL_WORDS; i++) t[i] = 0;
    for (int i = 0; i < BN_WORDS; i++) {
        for (int j = 0; j < BN_WORDS; j++) {
            t[i + j] += (uint64_t)a->d[i] * b->d[j];
        }
    }
    uint64_t carry = 0;
    for (int i = 0; i < BN_MUL_WORDS; i++) {
        uint64_t sum = t[i] + carry;
        r->d[i] = (uint16_t)(sum & 0xFFFF);
        carry = sum >> 16;
    }
}

static int bn_mul_bitlen(const bn_mul_t* a) {
    for (int i = BN_MUL_WORDS - 1; i >= 0; i--) {
        if (a->d[i]) {
            uint16_t v = a->d[i];
            int b = 0;
            while (v) { b++; v >>= 1; }
            return i * 16 + b;
        }
    }
    return 0;
}

static int bn_mul_geq(const bn_mul_t* a, const bn_mul_t* b) {
    for (int i = BN_MUL_WORDS - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) return (a->d[i] > b->d[i]) ? 1 : -1;
    }
    return 1;
}

static void bn_mul_sub(bn_mul_t* a, const bn_mul_t* b) {
    int borrow = 0;
    for (int i = 0; i < BN_MUL_WORDS; i++) {
        int sub = b->d[i] + borrow;
        int res = a->d[i] - sub;
        if (res < 0) { res += 65536; borrow = 1; } else { borrow = 0; }
        a->d[i] = (uint16_t)res;
    }
}

static void bn_mul_shr1(bn_mul_t* a) {
    uint16_t carry = 0;
    for (int i = BN_MUL_WORDS - 1; i >= 0; i--) {
        uint16_t v = a->d[i];
        a->d[i] = (v >> 1) | carry;
        carry = (v & 1) << 15;
    }
}

static void bn_mod(bn_t* r, const bn_mul_t* a, const bn_t* mod) {
    bn_mul_t x;
    for (int i = 0; i < BN_MUL_WORDS; i++) x.d[i] = a->d[i];

    int mod_bits = bn_bitlen(mod);
    if (mod_bits == 0) { bn_zero(r); return; }

    int x_bits = bn_mul_bitlen(&x);
    if (x_bits < mod_bits) {
        for (int i = 0; i < BN_WORDS; i++) r->d[i] = x.d[i];
        return;
    }

    bn_mul_t m;
    for (int i = 0; i < BN_MUL_WORDS; i++) m.d[i] = 0;
    for (int i = 0; i < BN_WORDS; i++) m.d[i] = mod->d[i];

    int shift = x_bits - mod_bits;
    int word_shift = shift / 16;
    int bit_shift = shift % 16;
    if (word_shift >= BN_MUL_WORDS) {
        for (int i = 0; i < BN_WORDS; i++) r->d[i] = x.d[i];
        return;
    }

    if (bit_shift == 0) {
        for (int i = BN_MUL_WORDS - 1; i >= word_shift; i--) {
            m.d[i] = m.d[i - word_shift];
        }
        for (int i = 0; i < word_shift; i++) m.d[i] = 0;
    } else {
        for (int i = BN_MUL_WORDS - 1; i > word_shift; i--) {
            m.d[i] = (uint16_t)(((uint32_t)m.d[i - word_shift] << bit_shift) |
                                (m.d[i - word_shift - 1] >> (16 - bit_shift)));
        }
        m.d[word_shift] = (uint16_t)((uint32_t)m.d[0] << bit_shift);
        for (int i = 0; i < word_shift; i++) m.d[i] = 0;
    }

    for (int s = shift; s >= 0; s--) {
        if (bn_mul_geq(&x, &m) >= 0) bn_mul_sub(&x, &m);
        bn_mul_shr1(&m);
    }

    for (int i = 0; i < BN_WORDS; i++) r->d[i] = x.d[i];
}

static void bn_mod_mul(bn_t* r, const bn_t* a, const bn_t* b, const bn_t* mod) {
    bn_mul_t p;
    bn_mul(&p, a, b);
    bn_mod(r, &p, mod);
}

static void bn_exp_mod(bn_t* out, const bn_t* base, const bn_t* exp, const bn_t* mod) {
    bn_t result;
    bn_zero(&result);
    result.d[0] = 1;

    bn_t b, e;
    bn_cpy(&b, base);
    bn_cpy(&e, exp);

    while (!bn_is_zero(&e)) {
        if (e.d[0] & 1) bn_mod_mul(&result, &result, &b, mod);
        bn_mod_mul(&b, &b, &b, mod);
        bn_shr1(&e);
    }
    bn_cpy(out, &result);
}

int rsa_encrypt(const uint8_t* mod, int mod_len,
                const uint8_t* exp, int exp_len,
                const uint8_t* in, int in_len,
                uint8_t* out, int out_max) {
    if (!mod || !exp || !in || !out) return -1;
    if (mod_len <= 0 || mod_len > BN_WORDS * 2) return -1;
    if (out_max < mod_len) return -1;

    bn_t mod_bn, exp_bn, in_bn, out_bn;
    bn_from_bytes(&mod_bn, mod, mod_len);
    bn_from_bytes(&exp_bn, exp, exp_len);
    bn_from_bytes(&in_bn, in, in_len);

    bn_exp_mod(&out_bn, &in_bn, &exp_bn, &mod_bn);

    bn_to_bytes(&out_bn, out, mod_len);
    return mod_len;
}
