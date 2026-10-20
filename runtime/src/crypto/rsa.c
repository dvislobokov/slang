/* rsa.c - RSA signatures (RFC 8017): RSASSA-PKCS1-v1_5 and RSASSA-PSS
 * (MGF1, same hash) verification and signing.
 *
 * Bignums are little-endian arrays of 64-bit limbs (up to 8192-bit moduli)
 * with Montgomery multiplication (CIOS). Public operations (verification)
 * handle only public data and use plain square-and-multiply. Private
 * operations use CRT with
 *   - exponent blinding: d_p + r (p - 1), d_q + r' (q - 1) with random
 *     64-bit r, r', so the exponent bits differ on every signature;
 *   - a fixed 4-bit-window exponentiation whose table lookups scan every
 *     entry with masks (no secret-dependent branches or indices);
 *   - verification of the result with the public exponent before it is
 *     released (protects against fault attacks on CRT).
 * The private key is a PKCS#1 RSAPrivateKey DER (RFC 8017 A.1.2). */
#include "crypto.h"

#include <string.h>

typedef unsigned __int128 u128;

#define MAXL 130   /* limbs: 8192-bit modulus + slack */

typedef struct {
    int nl;               /* limbs of the modulus */
    uint64_t m[MAXL];
    uint64_t m0;          /* -m^-1 mod 2^64 */
    uint64_t rr[MAXL];    /* R^2 mod m */
} mctx;

/* ---- limb helpers ---- */

static bool bn_from_be(uint64_t *r, int nl, const uint8_t *b, size_t len) {
    while (len && *b == 0) { b++; len--; }
    if (len > (size_t)nl * 8) return false;
    memset(r, 0, (size_t)nl * 8);
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        r[bit / 64] |= (uint64_t)b[i] << (bit % 64);
    }
    return true;
}

static void bn_to_be(uint8_t *b, size_t len, const uint64_t *a, int nl) {
    for (size_t i = 0; i < len; i++) {
        size_t bit = (len - 1 - i) * 8;
        b[i] = bit / 64 < (size_t)nl ? (uint8_t)(a[bit / 64] >> (bit % 64)) : 0;
    }
}

static uint64_t bn_sub(uint64_t *r, const uint64_t *a, const uint64_t *b, int nl) {
    uint64_t borrow = 0;
    for (int i = 0; i < nl; i++) {
        u128 t = (u128)a[i] - b[i] - borrow;
        r[i] = (uint64_t)t;
        borrow = (uint64_t)(t >> 64) & 1;
    }
    return borrow;
}

static uint64_t bn_add(uint64_t *r, const uint64_t *a, const uint64_t *b, int nl) {
    uint64_t carry = 0;
    for (int i = 0; i < nl; i++) {
        u128 t = (u128)a[i] + b[i] + carry;
        r[i] = (uint64_t)t;
        carry = (uint64_t)(t >> 64);
    }
    return carry;
}

static void bn_cmov(uint64_t *r, const uint64_t *a, int nl, uint64_t mask) {
    for (int i = 0; i < nl; i++) r[i] ^= (r[i] ^ a[i]) & mask;
}


static int bn_bits(const uint64_t *a, int nl) {
    for (int i = nl - 1; i >= 0; i--)
        if (a[i]) return i * 64 + 64 - __builtin_clzll(a[i]);
    return 0;
}

/* ---- Montgomery ---- */

static void mmul(const mctx *c, uint64_t *r, const uint64_t *a, const uint64_t *b);

static bool mctx_init(mctx *c, const uint64_t *m, int nl) {
    if (nl < 1 || nl > MAXL - 2 || !(m[0] & 1)) return false;
    memset(c, 0, sizeof *c);
    c->nl = nl;
    memcpy(c->m, m, (size_t)nl * 8);
    uint64_t inv = 1;
    for (int i = 0; i < 7; i++) inv *= 2 - m[0] * inv;
    c->m0 = 0 - inv;
    int mb = bn_bits(m, nl), r = 64 * nl;
    if (mb < 2) return false;
    /* x = R mod m: start from 2^(mb-1) < m and double up to 2^r */
    uint64_t x[MAXL] = { 0 }, t[MAXL], y[MAXL];
    x[(mb - 1) / 64] = (uint64_t)1 << ((mb - 1) % 64);
    for (int i = mb - 1; i < r + 1; i++) {
        if (i == r) memcpy(y, x, (size_t)nl * 8);   /* keep R mod m */
        uint64_t carry = bn_add(x, x, x, nl);
        uint64_t borrow = bn_sub(t, x, c->m, nl);
        bn_cmov(x, t, nl, 0 - (carry | (borrow ^ 1)));
    }
    /* now y = R mod m = Mont(1) and x = 2R mod m = Mont(2); Mont(2)^r = Mont(2^r) = R^2 mod m */
    uint64_t acc[MAXL];
    memcpy(acc, y, (size_t)nl * 8);
    for (int b = 31 - __builtin_clz((unsigned)r); b >= 0; b--) {
        mmul(c, acc, acc, acc);
        if ((r >> b) & 1) mmul(c, acc, acc, x);
    }
    memcpy(c->rr, acc, (size_t)nl * 8);
    return true;
}

/* r = a b R^-1 mod m (a, b < m); r may alias a or b. */
static void mmul(const mctx *c, uint64_t *r, const uint64_t *a, const uint64_t *b) {
    int nl = c->nl;
    uint64_t t[MAXL + 2];
    memset(t, 0, (size_t)(nl + 2) * 8);
    for (int i = 0; i < nl; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < nl; j++) {
            u128 x = (u128)a[j] * b[i] + t[j] + carry;
            t[j] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        u128 x = (u128)t[nl] + carry;
        t[nl] = (uint64_t)x;
        t[nl + 1] = (uint64_t)(x >> 64);
        uint64_t mm = t[0] * c->m0;
        x = (u128)mm * c->m[0] + t[0];
        carry = (uint64_t)(x >> 64);
        for (int j = 1; j < nl; j++) {
            x = (u128)mm * c->m[j] + t[j] + carry;
            t[j - 1] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        x = (u128)t[nl] + carry;
        t[nl - 1] = (uint64_t)x;
        t[nl] = t[nl + 1] + (uint64_t)(x >> 64);
    }
    uint64_t d[MAXL];
    uint64_t borrow = bn_sub(d, t, c->m, nl);
    bn_cmov(t, d, nl, 0 - (t[nl] | (borrow ^ 1)));
    memcpy(r, t, (size_t)nl * 8);
}

/* r = T mod m for a double-length T (2 nl limbs, T < m R). */
static void mreduce_wide(const mctx *c, uint64_t *r, const uint64_t *T) {
    int nl = c->nl;
    uint64_t t[2 * MAXL + 1];
    memcpy(t, T, (size_t)2 * nl * 8);
    t[2 * nl] = 0;
    for (int i = 0; i < nl; i++) {
        uint64_t mm = t[i] * c->m0, carry = 0;
        for (int j = 0; j < nl; j++) {
            u128 x = (u128)mm * c->m[j] + t[i + j] + carry;
            t[i + j] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        for (int k = i + nl; k <= 2 * nl; k++) {   /* full length: no data-dependent exit */
            u128 x = (u128)t[k] + carry;
            t[k] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
    }
    uint64_t *hi = t + nl, d[MAXL];
    uint64_t borrow = bn_sub(d, hi, c->m, nl);
    bn_cmov(hi, d, nl, 0 - (t[2 * nl] | (borrow ^ 1)));
    /* hi = T R^-1; multiply by R^2 to get T mod m */
    mmul(c, r, hi, c->rr);
}

/* r = a^e mod m with a public exponent (variable time). a < m, normal form. */
static void modexp_pub(const mctx *c, uint64_t *r, const uint64_t *a, const uint64_t *e, int el) {
    int nl = c->nl;
    uint64_t am[MAXL], acc[MAXL], one[MAXL] = { 1 };
    mmul(c, am, a, c->rr);
    mmul(c, acc, one, c->rr);
    for (int i = bn_bits(e, el) - 1; i >= 0; i--) {
        mmul(c, acc, acc, acc);
        if ((e[i / 64] >> (i % 64)) & 1) mmul(c, acc, acc, am);
    }
    mmul(c, r, acc, one);
    (void)nl;
}

/* r = a^e mod m in constant time for the el limbs of e (secret exponent). */
static void modexp_ct(const mctx *c, uint64_t *r, const uint64_t *a, const uint64_t *e, int el) {
    int nl = c->nl;
    static const int W = 4;
    uint64_t tab[16][MAXL], acc[MAXL], sel[MAXL], one[MAXL] = { 1 };
    mmul(c, tab[0], one, c->rr);
    mmul(c, tab[1], a, c->rr);
    for (int i = 2; i < 16; i++) mmul(c, tab[i], tab[i - 1], tab[1]);
    memcpy(acc, tab[0], (size_t)nl * 8);
    for (int w = el * 64 / W - 1; w >= 0; w--) {
        for (int k = 0; k < W; k++) mmul(c, acc, acc, acc);
        unsigned nib = (unsigned)(e[(w * W) / 64] >> ((w * W) % 64)) & 15;
        memset(sel, 0, (size_t)nl * 8);
        for (unsigned i = 0; i < 16; i++) {
            uint64_t eq = ((uint64_t)(i ^ nib) - 1) >> 63;
            bn_cmov(sel, tab[i], nl, 0 - eq);
        }
        mmul(c, acc, acc, sel);
    }
    mmul(c, r, acc, one);
    slang__crypto_wipe(tab, sizeof tab);
    slang__crypto_wipe(sel, sizeof sel);
}

/* ---- encodings ---- */

static const uint8_t DI_SHA1[] = { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a, 0x05, 0x00, 0x04, 0x14 };
static const uint8_t DI_SHA256[] = { 0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20 };
static const uint8_t DI_SHA384[] = { 0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30 };
static const uint8_t DI_SHA512[] = { 0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
                                     0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40 };

/* EMSA-PKCS1-v1_5 encoding of a digest into em (k bytes). */
static bool emsa_pkcs1(uint8_t *em, size_t k, int alg, const uint8_t *digest, size_t dlen) {
    const uint8_t *di;
    size_t dil;
    switch (alg) {
    case SLANG_HASH_SHA1: di = DI_SHA1; dil = sizeof DI_SHA1; break;
    case SLANG_HASH_SHA256: di = DI_SHA256; dil = sizeof DI_SHA256; break;
    case SLANG_HASH_SHA384: di = DI_SHA384; dil = sizeof DI_SHA384; break;
    case SLANG_HASH_SHA512: di = DI_SHA512; dil = sizeof DI_SHA512; break;
    default: return false;
    }
    if (dlen != slang__hash_size(alg) || k < dil + dlen + 11) return false;
    em[0] = 0;
    em[1] = 1;
    size_t ps = k - 3 - dil - dlen;
    memset(em + 2, 0xff, ps);
    em[2 + ps] = 0;
    memcpy(em + 3 + ps, di, dil);
    memcpy(em + 3 + ps + dil, digest, dlen);
    return true;
}

static void mgf1_xor(int alg, uint8_t *out, size_t n, const uint8_t *seed, size_t slen) {
    size_t hl = slang__hash_size(alg);
    uint8_t ctr[4], h[64];
    for (uint32_t i = 0; n; i++) {
        slang__st32be(ctr, i);
        slang_hash_ctx c;
        slang__hash_init(&c, alg);
        slang__hash_update(&c, seed, slen);
        slang__hash_update(&c, ctr, 4);
        slang__hash_final(&c, h);
        size_t k = n < hl ? n : hl;
        for (size_t j = 0; j < k; j++) out[j] ^= h[j];
        out += k;
        n -= k;
    }
}

/* EMSA-PSS-ENCODE with sLen = hLen; em has emLen = ceil((modbits-1)/8) bytes. */
static bool emsa_pss_encode(uint8_t *em, size_t emlen, size_t embits, int alg, const uint8_t *digest,
                            size_t dlen) {
    size_t hl = slang__hash_size(alg);
    if (hl == 0 || dlen != hl || emlen < 2 * hl + 2) return false;
    uint8_t salt[64], m1[8 + 64 + 64] = { 0 }, H[64];
    if (!slang__crypto_random(salt, hl)) return false;
    memcpy(m1 + 8, digest, hl);
    memcpy(m1 + 8 + hl, salt, hl);
    slang__hash(alg, m1, 8 + 2 * hl, H);
    size_t dblen = emlen - hl - 1;
    memset(em, 0, dblen);
    em[dblen - hl - 1] = 1;
    memcpy(em + dblen - hl, salt, hl);
    mgf1_xor(alg, em, dblen, H, hl);
    em[0] &= (uint8_t)(0xff >> (8 * emlen - embits));
    memcpy(em + dblen, H, hl);
    em[emlen - 1] = 0xbc;
    return true;
}

static bool emsa_pss_verify(const uint8_t *emin, size_t emlen, size_t embits, int alg, const uint8_t *digest,
                            size_t dlen, int saltlen) {
    size_t hl = slang__hash_size(alg);
    if (hl == 0 || dlen != hl || emlen < hl + 2 || emlen > 1024) return false;
    uint8_t em[1024];
    memcpy(em, emin, emlen);
    if (em[emlen - 1] != 0xbc) return false;
    size_t dblen = emlen - hl - 1;
    const uint8_t *H = em + dblen;
    uint8_t topmask = (uint8_t)(0xff << (8 - (8 * emlen - embits)));
    if (8 * emlen - embits && (em[0] & topmask)) return false;
    mgf1_xor(alg, em, dblen, H, hl);
    em[0] &= (uint8_t)(0xff >> (8 * emlen - embits));
    size_t i = 0;
    while (i < dblen && em[i] == 0) i++;
    if (i == dblen || em[i] != 1) return false;
    size_t slen = dblen - i - 1;
    if (saltlen >= 0 && slen != (size_t)saltlen) return false;
    uint8_t m1[8 + 64 + 1024], h2[64];
    memset(m1, 0, 8);
    memcpy(m1 + 8, digest, hl);
    memcpy(m1 + 8 + hl, em + i + 1, slen);
    slang__hash(alg, m1, 8 + hl + slen, h2);
    return slang__crypto_ct_eq(h2, H, hl) == 1;
}

/* ---- public operations ---- */

typedef struct {
    mctx n;
    uint64_t e[MAXL];
    int el;
    size_t k;       /* modulus bytes */
    int bits;
} pubkey;

static bool pub_load(pubkey *pk, const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen) {
    while (nlen && *n == 0) { n++; nlen--; }
    while (elen && *e == 0) { e++; elen--; }
    if (nlen < 64 || nlen > 1024 || elen == 0 || elen > 64) return false;   /* 512..8192 bits */
    int nl = (int)((nlen + 7) / 8);
    uint64_t m[MAXL];
    bn_from_be(m, nl, n, nlen);
    if (!mctx_init(&pk->n, m, nl)) return false;
    pk->el = (int)((elen + 7) / 8);
    bn_from_be(pk->e, pk->el, e, elen);
    pk->k = nlen;
    pk->bits = bn_bits(m, nl);
    return true;
}

/* m = s^e mod n as k bytes; false if s >= n. */
static bool pub_op(const pubkey *pk, const uint8_t *sig, size_t slen, uint8_t *out) {
    if (slen != pk->k) return false;
    int nl = pk->n.nl;
    uint64_t s[MAXL], t[MAXL], r[MAXL];
    bn_from_be(s, nl, sig, slen);
    if (!bn_sub(t, s, pk->n.m, nl)) return false;   /* s >= n */
    modexp_pub(&pk->n, r, s, pk->e, pk->el);
    bn_to_be(out, pk->k, r, nl);
    return true;
}

bool slang__rsa_public(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, const uint8_t *in,
                       size_t ilen, uint8_t *out) {
    pubkey pk;
    return pub_load(&pk, n, nlen, e, elen) && pub_op(&pk, in, ilen, out);
}

bool slang__rsa_verify_pkcs1(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int alg,
                             const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen) {
    pubkey pk;
    if (!pub_load(&pk, n, nlen, e, elen)) return false;
    uint8_t m[1024], want[1024];
    if (!pub_op(&pk, sig, slen, m)) return false;
    if (!emsa_pkcs1(want, pk.k, alg, digest, dlen)) return false;
    return slang__crypto_ct_eq(m, want, pk.k) == 1;
}

bool slang__rsa_verify_pss(const uint8_t *n, size_t nlen, const uint8_t *e, size_t elen, int alg,
                           const uint8_t *digest, size_t dlen, const uint8_t *sig, size_t slen, int saltlen) {
    pubkey pk;
    if (!pub_load(&pk, n, nlen, e, elen)) return false;
    uint8_t m[1024];
    if (!pub_op(&pk, sig, slen, m)) return false;
    size_t embits = (size_t)pk.bits - 1, emlen = (embits + 7) / 8;
    /* the integer must fit in emLen bytes */
    for (size_t i = 0; i < pk.k - emlen; i++)
        if (m[i]) return false;
    return emsa_pss_verify(m + (pk.k - emlen), emlen, embits, alg, digest, dlen, saltlen);
}

/* ---- PKCS#1 private key DER ---- */

typedef struct {
    const uint8_t *p;
    size_t len;
} span;

/* Reads one DER TLV of tag `tag` from *in; false on malformed input. */
static bool der_next(span *in, uint8_t tag, span *out) {
    if (in->len < 2 || in->p[0] != tag) return false;
    size_t len = in->p[1], hdr = 2;
    if (len & 0x80) {
        size_t nb = len & 0x7f;
        if (nb == 0 || nb > 3 || in->len < 2 + nb) return false;
        len = 0;
        for (size_t i = 0; i < nb; i++) len = len << 8 | in->p[2 + i];
        hdr = 2 + nb;
    }
    if (in->len - hdr < len) return false;
    out->p = in->p + hdr;
    out->len = len;
    in->p += hdr + len;
    in->len -= hdr + len;
    return true;
}

typedef struct {
    span n, e, d, p, q, dp, dq, qinv;
} privkey_der;

static bool priv_parse(const uint8_t *der, size_t derlen, privkey_der *k) {
    span all = { der, derlen }, seq, ver;
    if (!der_next(&all, 0x30, &seq)) return false;
    if (!der_next(&seq, 0x02, &ver) || ver.len != 1 || ver.p[0] != 0) return false;
    span *f[8] = { &k->n, &k->e, &k->d, &k->p, &k->q, &k->dp, &k->dq, &k->qinv };
    for (int i = 0; i < 8; i++) {
        if (!der_next(&seq, 0x02, f[i])) return false;
        while (f[i]->len > 1 && f[i]->p[0] == 0) { f[i]->p++; f[i]->len--; }
    }
    return true;
}

size_t slang__rsa_priv_size(const uint8_t *der, size_t derlen) {
    privkey_der k;
    if (!priv_parse(der, derlen, &k)) return 0;
    return k.n.len;
}

/* s = m^d mod n by CRT with blinding and verification; in/out are k bytes. */
static bool priv_op(const uint8_t *der, size_t derlen, const uint8_t *in, uint8_t *out, size_t *klen) {
    privkey_der kd;
    if (!priv_parse(der, derlen, &kd)) return false;
    pubkey pk;
    if (!pub_load(&pk, kd.n.p, kd.n.len, kd.e.p, kd.e.len)) return false;
    size_t k = pk.k;
    int hl = (int)((kd.p.len > kd.q.len ? kd.p.len : kd.q.len) + 7) / 8;
    if (hl > MAXL / 2 - 2) return false;
    uint64_t pp[MAXL] = { 0 }, qq[MAXL] = { 0 };
    if (!bn_from_be(pp, hl, kd.p.p, kd.p.len) || !bn_from_be(qq, hl, kd.q.p, kd.q.len)) return false;
    mctx cp, cq;
    if (!mctx_init(&cp, pp, hl) || !mctx_init(&cq, qq, hl)) return false;
    uint64_t c[MAXL] = { 0 };
    bn_from_be(c, 2 * hl, in, k);
    {
        uint64_t t[MAXL];
        uint64_t nn[MAXL] = { 0 };
        memcpy(nn, pk.n.m, (size_t)pk.n.nl * 8);
        if (!bn_sub(t, c, nn, 2 * hl)) return false;   /* input >= n */
    }
    /* blinded exponents dp + r (p - 1), dq + r' (q - 1), hl + 1 limbs */
    uint64_t r2[2];
    if (!slang__crypto_random((uint8_t *)r2, sizeof r2)) return false;
    uint64_t dpb[MAXL] = { 0 }, dqb[MAXL] = { 0 };
    if (!bn_from_be(dpb, hl + 1, kd.dp.p, kd.dp.len) || !bn_from_be(dqb, hl + 1, kd.dq.p, kd.dq.len)) return false;
    for (int which = 0; which < 2; which++) {
        uint64_t *db = which ? dqb : dpb, *pr = which ? qq : pp, rnd = r2[which] | 1;
        uint64_t pm1[MAXL];
        memcpy(pm1, pr, (size_t)hl * 8);
        pm1[0] &= ~(uint64_t)1;   /* p odd: p - 1 */
        uint64_t carry = 0;
        for (int i = 0; i <= hl; i++) {
            u128 x = (u128)(i < hl ? pm1[i] : 0) * rnd + db[i] + carry;
            db[i] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
    }
    uint64_t cmp[MAXL], cmq[MAXL], m1[MAXL], m2[MAXL];
    mreduce_wide(&cp, cmp, c);
    mreduce_wide(&cq, cmq, c);
    modexp_ct(&cp, m1, cmp, dpb, hl + 1);
    modexp_ct(&cq, m2, cmq, dqb, hl + 1);
    /* h = qinv (m1 - m2) mod p */
    uint64_t wide[2 * MAXL] = { 0 }, m2p[MAXL] = { 0 }, diff[MAXL] = { 0 }, qinv[MAXL] = { 0 }, h[MAXL] = { 0 }, tmp[MAXL] = { 0 };
    memcpy(wide, m2, (size_t)hl * 8);
    mreduce_wide(&cp, m2p, wide);
    uint64_t borrow = bn_sub(diff, m1, m2p, hl);
    bn_add(tmp, diff, pp, hl);
    bn_cmov(diff, tmp, hl, 0 - borrow);
    if (!bn_from_be(qinv, hl, kd.qinv.p, kd.qinv.len)) return false;
    mmul(&cp, tmp, qinv, diff);
    mmul(&cp, h, tmp, cp.rr);
    /* s = m2 + h q */
    uint64_t s[2 * MAXL + 1] = { 0 };
    for (int i = 0; i < hl; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < hl; j++) {
            u128 x = (u128)h[i] * qq[j] + s[i + j] + carry;
            s[i + j] = (uint64_t)x;
            carry = (uint64_t)(x >> 64);
        }
        s[i + hl] = carry;
    }
    uint64_t carry = 0;
    for (int i = 0; i < 2 * hl + 1; i++) {
        u128 x = (u128)s[i] + (i < hl ? m2[i] : 0) + carry;
        s[i] = (uint64_t)x;
        carry = (uint64_t)(x >> 64);
    }
    bn_to_be(out, k, s, 2 * hl + 1);
    /* verify: s^e mod n == input */
    uint8_t chk[1024];
    if (!pub_op(&pk, out, k, chk) || !slang__crypto_ct_eq(chk, in, k)) {
        memset(out, 0, k);
        return false;
    }
    slang__crypto_wipe(dpb, sizeof dpb);
    slang__crypto_wipe(dqb, sizeof dqb);
    slang__crypto_wipe(m1, sizeof m1);
    slang__crypto_wipe(m2, sizeof m2);
    slang__crypto_wipe(h, sizeof h);
    *klen = k;
    return true;
}

bool slang__rsa_sign_pkcs1(const uint8_t *der, size_t derlen, int alg, const uint8_t *digest, size_t dlen,
                           uint8_t *sig, size_t *siglen) {
    size_t k = slang__rsa_priv_size(der, derlen);
    uint8_t em[1024];
    if (k == 0 || k > 1024 || !emsa_pkcs1(em, k, alg, digest, dlen)) return false;
    return priv_op(der, derlen, em, sig, siglen);
}

bool slang__rsa_sign_pss(const uint8_t *der, size_t derlen, int alg, const uint8_t *digest, size_t dlen,
                         uint8_t *sig, size_t *siglen) {
    privkey_der kd;
    if (!priv_parse(der, derlen, &kd) || kd.n.len > 1024 || kd.n.len < 64) return false;
    uint64_t nb[MAXL];
    int nl = (int)((kd.n.len + 7) / 8);
    bn_from_be(nb, nl, kd.n.p, kd.n.len);
    size_t bits = (size_t)bn_bits(nb, nl), k = kd.n.len;
    size_t embits = bits - 1, emlen = (embits + 7) / 8;
    uint8_t em[1024] = { 0 };
    if (!emsa_pss_encode(em + (k - emlen), emlen, embits, alg, digest, dlen)) return false;
    return priv_op(der, derlen, em, sig, siglen);
}
