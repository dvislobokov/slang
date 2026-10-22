/* std_math_big.c - natural-number (magnitude) arithmetic for math.big.
 *
 * A magnitude is a Zell []uint32 of little-endian base-2^32 limbs with no
 * high zero limbs (zero is the empty slice). Every function here takes
 * normalized magnitudes (borrowed, NULL = zero) and writes a new
 * normalized one to `out` (owned, ABI.md 14.1). Signs, Rat and Decimal are
 * Zell (compiler/src/sema/stdlib_src/big.zl).
 *
 * Multiplication is schoolbook below KARATSUBA_MIN limbs and Karatsuba
 * above; division is Knuth's algorithm D. */
#include "rt_internal.h"

#include <string.h>

void zell_math_big_rt_add(const zell_slice *a, const zell_slice *b, zell_slice *out);
void zell_math_big_rt_sub(const zell_slice *a, const zell_slice *b, zell_slice *out);
int64_t zell_math_big_rt_cmp(const zell_slice *a, const zell_slice *b);
void zell_math_big_rt_mul(const zell_slice *a, const zell_slice *b, zell_slice *out);
void zell_math_big_rt_divmod(const zell_slice *a, const zell_slice *b, zell_slice *out);
void zell_math_big_rt_shl(const zell_slice *a, int64_t n, zell_slice *out);
void zell_math_big_rt_shr(const zell_slice *a, int64_t n, zell_slice *out);
int64_t zell_math_big_rt_bit_len(const zell_slice *a);
zell_string *zell_math_big_rt_format(const zell_slice *a, int64_t base);
void zell_math_big_rt_parse(const zell_string *s, int64_t base, zell_slice *out, zell_error **err);
void zell_math_big_rt_from_bytes(const zell_slice *b, zell_slice *out);
void zell_math_big_rt_to_bytes(const zell_slice *a, zell_slice *out);
void zell_math_big_rt_from_uint(uint64_t x, zell_slice *out);

typedef uint32_t limb;

static int64_t nlen(const zell_slice *a) { return a ? a->len : 0; }
static const limb *ndata(const zell_slice *a) { return a && a->len ? (const limb *)a->data : NULL; }

static int64_t norm(const limb *p, int64_t n) {
    while (n > 0 && p[n - 1] == 0) n--;
    return n;
}

/* Writes the normalized copy of p[0..n) to out. */
static void emit(zell_slice *out, const limb *p, int64_t n) {
    n = norm(p, n);
    if (n == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 4, n, n);
    memcpy(out->data, p, (size_t)n * 4);
}

static limb *alloc_limbs(int64_t n) {
    return zell_alloc((n > 0 ? n : 1) * 4);
}

static int cmp_raw(const limb *a, int64_t an, const limb *b, int64_t bn) {
    if (an != bn) return an < bn ? -1 : 1;
    for (int64_t i = an - 1; i >= 0; i--) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* r[0..max(an,bn)+1) = a + b. */
static void add_raw(limb *r, const limb *a, int64_t an, const limb *b, int64_t bn) {
    if (an < bn) {
        const limb *t = a; a = b; b = t;
        int64_t tn = an; an = bn; bn = tn;
    }
    uint64_t c = 0;
    for (int64_t i = 0; i < an; i++) {
        c += (uint64_t)a[i] + (i < bn ? b[i] : 0);
        r[i] = (limb)c;
        c >>= 32;
    }
    r[an] = (limb)c;
}

/* r[0..an) = a - b, requires a >= b. Returns the final borrow (0). */
static limb sub_raw(limb *r, const limb *a, int64_t an, const limb *b, int64_t bn) {
    int64_t borrow = 0;
    for (int64_t i = 0; i < an; i++) {
        int64_t d = (int64_t)a[i] - (i < bn ? b[i] : 0) - borrow;
        borrow = d < 0;
        r[i] = (limb)(d + (borrow ? ((int64_t)1 << 32) : 0));
    }
    return (limb)borrow;
}

void zell_math_big_rt_add(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    int64_t an = nlen(a), bn = nlen(b), n = (an > bn ? an : bn) + 1;
    limb *r = alloc_limbs(n);
    add_raw(r, ndata(a), an, ndata(b), bn);
    emit(out, r, n);
    zell_free(r);
}

void zell_math_big_rt_sub(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    int64_t an = nlen(a), bn = nlen(b);
    if (cmp_raw(ndata(a), an, ndata(b), bn) < 0) zell_panic("big: internal: negative magnitude");
    limb *r = alloc_limbs(an);
    sub_raw(r, ndata(a), an, ndata(b), bn);
    emit(out, r, an);
    zell_free(r);
}

int64_t zell_math_big_rt_cmp(const zell_slice *a, const zell_slice *b) {
    return cmp_raw(ndata(a), nlen(a), ndata(b), nlen(b));
}

/* r[0..an+bn) += a * b (schoolbook; r must be zeroed by the caller). */
static void mul_school(limb *r, const limb *a, int64_t an, const limb *b, int64_t bn) {
    for (int64_t i = 0; i < an; i++) {
        uint64_t c = 0, ai = a[i];
        if (ai == 0) continue;
        for (int64_t j = 0; j < bn; j++) {
            c += ai * b[j] + r[i + j];
            r[i + j] = (limb)c;
            c >>= 32;
        }
        int64_t k = i + bn;
        while (c) {
            c += r[k];
            r[k] = (limb)c;
            c >>= 32;
            k++;
        }
    }
}

#define KARATSUBA_MIN 48

/* r[0..2n) = a * b with an, bn <= n (r zeroed). */
static void mul_rec(limb *r, const limb *a, int64_t an, const limb *b, int64_t bn) {
    if (an < KARATSUBA_MIN || bn < KARATSUBA_MIN) {
        mul_school(r, a, an, b, bn);
        return;
    }
    int64_t n = an > bn ? an : bn, h = n / 2;
    int64_t a0n = an < h ? an : h, a1n = an > h ? an - h : 0;
    int64_t b0n = bn < h ? bn : h, b1n = bn > h ? bn - h : 0;
    const limb *a0 = a, *a1 = a + h, *b0 = b, *b1 = b + h;
    /* z0 = a0*b0, z2 = a1*b1, z1 = (a0+a1)(b0+b1) - z0 - z2 */
    int64_t zn = 2 * (n - h) + 2;
    limb *z0 = alloc_limbs(2 * h), *z2 = alloc_limbs(zn), *z1 = alloc_limbs(zn + 2);
    limb *sa = alloc_limbs(n - h + 2), *sb = alloc_limbs(n - h + 2);
    mul_rec(z0, a0, norm(a0, a0n), b0, norm(b0, b0n));
    if (a1n && b1n) mul_rec(z2, a1, a1n, b1, b1n);
    add_raw(sa, a0, a0n, a1, a1n);
    add_raw(sb, b0, b0n, b1, b1n);
    int64_t san = norm(sa, (a0n > a1n ? a0n : a1n) + 1), sbn = norm(sb, (b0n > b1n ? b0n : b1n) + 1);
    mul_rec(z1, sa, san, sb, sbn);
    int64_t z1n = norm(z1, zn + 2);
    int64_t z0n = norm(z0, 2 * h), z2n = norm(z2, zn);
    sub_raw(z1, z1, z1n, z0, z0n);
    z1n = norm(z1, z1n);
    sub_raw(z1, z1, z1n, z2, z2n);
    z1n = norm(z1, z1n);
    /* r = z0 + z1 << 32h + z2 << 64h */
    memcpy(r, z0, (size_t)z0n * 4);
    int64_t rn = an + bn;
    uint64_t c = 0;
    for (int64_t i = 0; i < z2n; i++) {
        r[2 * h + i] = z2[i];
    }
    for (int64_t i = 0; h + i < rn; i++) {
        c += (uint64_t)r[h + i] + (i < z1n ? z1[i] : 0);
        r[h + i] = (limb)c;
        c >>= 32;
        if (i >= z1n && c == 0) break;
    }
    zell_free(z0);
    zell_free(z1);
    zell_free(z2);
    zell_free(sa);
    zell_free(sb);
}

void zell_math_big_rt_mul(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    int64_t an = nlen(a), bn = nlen(b);
    if (an == 0 || bn == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    int64_t n = an > bn ? an : bn;
    limb *r = alloc_limbs(2 * n + 2);
    mul_rec(r, ndata(a), an, ndata(b), bn);
    emit(out, r, an + bn);
    zell_free(r);
}

/* q = u / v, r = u % v (Knuth D). q has un - vn + 1 limbs, r has vn. */
static void divmod_raw(limb *q, limb *r, const limb *u0, int64_t un, const limb *v0, int64_t vn) {
    if (vn == 1) {
        uint64_t rem = 0, d = v0[0];
        for (int64_t i = un - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | u0[i];
            q[i] = (limb)(cur / d);
            rem = cur % d;
        }
        r[0] = (limb)rem;
        return;
    }
    int s = __builtin_clz(v0[vn - 1]);
    limb *v = alloc_limbs(vn), *u = alloc_limbs(un + 1);
    for (int64_t i = vn - 1; i > 0; i--) v[i] = (v0[i] << s) | (s ? (limb)((uint64_t)v0[i - 1] >> (32 - s)) : 0);
    v[0] = v0[0] << s;
    u[un] = s ? (limb)((uint64_t)u0[un - 1] >> (32 - s)) : 0;
    for (int64_t i = un - 1; i > 0; i--) u[i] = (u0[i] << s) | (s ? (limb)((uint64_t)u0[i - 1] >> (32 - s)) : 0);
    u[0] = u0[0] << s;
    const uint64_t B = (uint64_t)1 << 32;
    for (int64_t j = un - vn; j >= 0; j--) {
        uint64_t num = ((uint64_t)u[j + vn] << 32) | u[j + vn - 1];
        uint64_t qhat = num / v[vn - 1], rhat = num % v[vn - 1];
        while (qhat >= B || qhat * v[vn - 2] > ((rhat << 32) | u[j + vn - 2])) {
            qhat--;
            rhat += v[vn - 1];
            if (rhat >= B) break;
        }
        int64_t borrow = 0;
        uint64_t carry = 0;
        for (int64_t i = 0; i < vn; i++) {
            uint64_t p = qhat * v[i] + carry;
            carry = p >> 32;
            int64_t t = (int64_t)u[i + j] - (int64_t)(p & 0xFFFFFFFFu) - borrow;
            borrow = t < 0;
            u[i + j] = (limb)(t + (borrow ? (int64_t)B : 0));
        }
        int64_t t = (int64_t)u[j + vn] - (int64_t)carry - borrow;
        borrow = t < 0;
        u[j + vn] = (limb)(t + (borrow ? (int64_t)B : 0));
        if (borrow) {
            qhat--;
            uint64_t c = 0;
            for (int64_t i = 0; i < vn; i++) {
                c += (uint64_t)u[i + j] + v[i];
                u[i + j] = (limb)c;
                c >>= 32;
            }
            u[j + vn] += (limb)c;
        }
        q[j] = (limb)qhat;
    }
    for (int64_t i = 0; i < vn; i++) r[i] = (u[i] >> s) | (s ? (limb)((uint64_t)u[i + 1] << (32 - s)) : 0);
    zell_free(u);
    zell_free(v);
}

/* out = [len(q), q limbs..., r limbs...] (q and r normalized). Panics on
 * division by zero. */
void zell_math_big_rt_divmod(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    int64_t an = nlen(a), bn = nlen(b);
    if (bn == 0) zell_panic("division by zero");
    limb *q, *r;
    int64_t qn, rn;
    if (cmp_raw(ndata(a), an, ndata(b), bn) < 0) {
        q = alloc_limbs(1);
        qn = 0;
        r = alloc_limbs(an);
        if (an) memcpy(r, ndata(a), (size_t)an * 4);
        rn = an;
    } else {
        qn = an - bn + 1;
        q = alloc_limbs(qn);
        r = alloc_limbs(bn);
        divmod_raw(q, r, ndata(a), an, ndata(b), bn);
        rn = bn;
    }
    qn = norm(q, qn);
    rn = norm(r, rn);
    zell_slice_new(out, 4, 1 + qn + rn, 1 + qn + rn);
    limb *o = out->data;
    o[0] = (limb)qn;
    if (qn) memcpy(o + 1, q, (size_t)qn * 4);
    if (rn) memcpy(o + 1 + qn, r, (size_t)rn * 4);
    zell_free(q);
    zell_free(r);
}

void zell_math_big_rt_shl(const zell_slice *a, int64_t n, zell_slice *out) {
    int64_t an = nlen(a);
    if (an == 0 || n <= 0) {
        emit(out, ndata(a), an);
        return;
    }
    int64_t w = n / 32, s = n % 32, rn = an + w + 1;
    limb *r = alloc_limbs(rn);
    const limb *p = ndata(a);
    for (int64_t i = 0; i < an; i++) {
        uint64_t x = (uint64_t)p[i] << s;
        r[i + w] |= (limb)x;
        r[i + w + 1] |= (limb)(x >> 32);
    }
    emit(out, r, rn);
    zell_free(r);
}

void zell_math_big_rt_shr(const zell_slice *a, int64_t n, zell_slice *out) {
    int64_t an = nlen(a), w = n / 32, s = n % 32;
    if (n <= 0) {
        emit(out, ndata(a), an);
        return;
    }
    if (w >= an) {
        memset(out, 0, sizeof *out);
        return;
    }
    int64_t rn = an - w;
    limb *r = alloc_limbs(rn);
    const limb *p = ndata(a);
    for (int64_t i = 0; i < rn; i++) {
        uint64_t x = p[i + w] >> s;
        if (s && i + w + 1 < an) x |= ((uint64_t)p[i + w + 1] << (32 - s)) & 0xFFFFFFFFu;
        r[i] = (limb)x;
    }
    emit(out, r, rn);
    zell_free(r);
}

int64_t zell_math_big_rt_bit_len(const zell_slice *a) {
    int64_t an = nlen(a);
    if (an == 0) return 0;
    return (an - 1) * 32 + (32 - __builtin_clz(ndata(a)[an - 1]));
}

zell_string *zell_math_big_rt_format(const zell_slice *a, int64_t base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (base < 2 || base > 36) zell_panic("big: illegal base");
    int64_t n = nlen(a);
    if (n == 0) return zell_string_from_cstr("0");
    /* chunk = base^k <= 2^32 */
    uint64_t chunk = (uint64_t)base;
    int k = 1;
    while (chunk * (uint64_t)base <= 0xFFFFFFFFu) {
        chunk *= (uint64_t)base;
        k++;
    }
    limb *t = alloc_limbs(n);
    memcpy(t, ndata(a), (size_t)n * 4);
    size_t cap = (size_t)n * 33 + 2, len = 0;
    char *buf = zell_alloc((int64_t)cap);
    while (n > 0) {
        uint64_t rem = 0;
        for (int64_t i = n - 1; i >= 0; i--) {
            uint64_t cur = (rem << 32) | t[i];
            t[i] = (limb)(cur / chunk);
            rem = cur % chunk;
        }
        n = norm(t, n);
        for (int j = 0; j < k && (n > 0 || rem > 0); j++) {
            buf[len++] = digits[rem % (uint64_t)base];
            rem /= (uint64_t)base;
        }
    }
    for (size_t i = 0; i < len / 2; i++) {
        char c = buf[i];
        buf[i] = buf[len - 1 - i];
        buf[len - 1 - i] = c;
    }
    zell_string *s = zell_string_new(buf, (int64_t)len);
    zell_free(buf);
    zell_free(t);
    return s;
}

/* Digits only (no sign or prefix); '_' is not accepted here. */
void zell_math_big_rt_parse(const zell_string *s, int64_t base, zell_slice *out, zell_error **err) {
    memset(out, 0, sizeof *out);
    if (err) *err = NULL;
    int64_t n = s ? s->len : 0;
    if (base < 2 || base > 36 || n == 0) goto bad;
    int64_t cap = (n * 6) / 32 + 2; /* base <= 36 < 2^6: at most 6 bits per digit */
    limb *r = alloc_limbs(cap);
    int64_t rn = 0;
    for (int64_t i = 0; i < n; i++) {
        char c = s->data[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= base) {
            zell_free(r);
            goto bad;
        }
        uint64_t c2 = (uint64_t)d;
        for (int64_t j = 0; j < rn; j++) {
            c2 += (uint64_t)r[j] * (uint64_t)base;
            r[j] = (limb)c2;
            c2 >>= 32;
        }
        if (c2) r[rn++] = (limb)c2;
    }
    emit(out, r, rn);
    zell_free(r);
    return;
bad:
    if (err) {
        zell_string *m = zell_string_from_cstr("invalid syntax");
        *err = zell_error_new(m);
        zell_release(m);
    }
}

/* Big-endian bytes -> magnitude. */
void zell_math_big_rt_from_bytes(const zell_slice *b, zell_slice *out) {
    int64_t n = b ? b->len : 0;
    const uint8_t *p = n ? b->data : NULL;
    int64_t rn = (n + 3) / 4;
    limb *r = alloc_limbs(rn);
    for (int64_t i = 0; i < n; i++) {
        int64_t bit = (n - 1 - i) * 8;
        r[bit / 32] |= (limb)p[i] << (bit % 32);
    }
    emit(out, r, rn);
    zell_free(r);
}

/* Magnitude -> minimal big-endian bytes (empty for zero). */
void zell_math_big_rt_to_bytes(const zell_slice *a, zell_slice *out) {
    int64_t bits = zell_math_big_rt_bit_len(a), n = (bits + 7) / 8;
    if (n == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 1, n, n);
    uint8_t *o = out->data;
    const limb *p = ndata(a);
    for (int64_t i = 0; i < n; i++) {
        int64_t bit = (n - 1 - i) * 8;
        o[i] = (uint8_t)(p[bit / 32] >> (bit % 32));
    }
}

void zell_math_big_rt_from_uint(uint64_t x, zell_slice *out) {
    limb r[2] = {(limb)x, (limb)(x >> 32)};
    emit(out, r, 2);
}
