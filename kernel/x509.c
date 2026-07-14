#include "x509.h"

static int asn1_length(const uint8_t* p, int len, int* out_len, int* hdr_len) {
    if (len < 2) return -1;
    if (!(p[1] & 0x80)) {
        *out_len = p[1];
        *hdr_len = 2;
        return 0;
    }
    int nb = p[1] & 0x7F;
    if (len < 2 + nb) return -1;
    int l = 0;
    for (int i = 0; i < nb; i++) {
        l = (l << 8) | p[2 + i];
    }
    *out_len = l;
    *hdr_len = 2 + nb;
    return 0;
}

static int asn1_skip(const uint8_t* p, int len, int* skipped) {
    if (len < 2) return -1;
    int hl, vl;
    if (asn1_length(p, len, &vl, &hl) < 0) return -1;
    if (len < hl + vl) return -1;
    *skipped = hl + vl;
    return 0;
}

static int asn1_int(const uint8_t* p, int len,
                    const uint8_t** out, int* out_len, int* total) {
    if (len < 2 || p[0] != 0x02) return -1;
    int hl, vl;
    if (asn1_length(p, len, &vl, &hl) < 0) return -1;
    if (len < hl + vl) return -1;
    *out = p + hl;
    *out_len = vl;
    if (total) *total = hl + vl;
    return 0;
}

static int asn1_sequence(const uint8_t* p, int len,
                         const uint8_t** out, int* out_len, int* total) {
    if (len < 2 || p[0] != 0x30) return -1;
    int hl, vl;
    if (asn1_length(p, len, &vl, &hl) < 0) return -1;
    if (len < hl + vl) return -1;
    *out = p + hl;
    *out_len = vl;
    if (total) *total = hl + vl;
    return 0;
}

int x509_parse_rsa_pubkey(const uint8_t* der, int der_len,
                          uint8_t* mod, int* mod_len, int mod_max,
                          uint8_t* exp, int* exp_len, int exp_max) {
    if (!der || der_len <= 0 || !mod || !mod_len || !exp || !exp_len) return -1;

    const uint8_t* p = der;
    int len = der_len;
    const uint8_t* body;
    int body_len;
    int total;

    if (asn1_sequence(p, len, &body, &body_len, &total) < 0) return -1;
    p = body; len = body_len;

    // TBSCertificate
    if (asn1_sequence(p, len, &body, &body_len, &total) < 0) return -1;
    p = body; len = body_len;

    // optional version [0]
    if (len > 0 && p[0] == 0xA0) {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // serialNumber
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // signature AlgorithmIdentifier
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // issuer Name
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // validity
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // subject Name
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // subjectPublicKeyInfo
    if (asn1_sequence(p, len, &body, &body_len, &total) < 0) return -1;
    p = body; len = body_len;

    // algorithm
    {
        int skipped;
        if (asn1_skip(p, len, &skipped) < 0) return -1;
        p += skipped; len -= skipped;
    }

    // subjectPublicKey BIT STRING
    if (len < 3 || p[0] != 0x03) return -1;
    int hl, vl;
    if (asn1_length(p, len, &vl, &hl) < 0) return -1;
    if (vl < 1 || len < hl + vl) return -1;
    const uint8_t* key_der = p + hl + 1;
    int key_der_len = vl - 1;

    // RSAPublicKey
    if (asn1_sequence(key_der, key_der_len, &body, &body_len, &total) < 0) return -1;
    p = body; len = body_len;

    // modulus (drop ASN.1 leading zero, but store exact byte length for TLS CKE padding)
    const uint8_t* m;
    int ml;
    if (asn1_int(p, len, &m, &ml, &total) < 0) return -1;
    if (ml > 0 && m[0] == 0) { m++; ml--; }
    if (ml > mod_max) return -1;
    for (int i = 0; i < ml; i++) mod[i] = m[i];
    *mod_len = ml;
    p += total; len -= total;

    // exponent
    const uint8_t* e;
    int el;
    if (asn1_int(p, len, &e, &el, &total) < 0) return -1;
    if (el > 0 && e[0] == 0) { e++; el--; }
    if (el > exp_max) return -1;
    for (int i = 0; i < el; i++) exp[i] = e[i];
    *exp_len = el;

    return 0;
}
