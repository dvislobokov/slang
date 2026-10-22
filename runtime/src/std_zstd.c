/* std_zstd.c - Zstandard decoder (RFC 8878) for std.compress.zstd
 * (runtime/ABI.md 19.2). Whole frames are decoded into memory: Raw, RLE
 * and Compressed blocks, Huffman-coded literals (1 or 4 streams, treeless
 * reuse), FSE-coded sequences (predefined, RLE, compressed and repeat
 * tables), repeat offsets, skippable frames and the XXH64 content checksum.
 * Dictionaries are not supported (a frame naming one is an error). */
#include "rt_internal.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *p;
    size_t n, cap;
} zbuf;

static int zb_grow(zbuf *b, size_t need) {
    if (b->n + need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 65536;
    while (cap < b->n + need) {
        if (cap > ((size_t)1 << 40)) return 0;
        cap *= 2;
    }
    uint8_t *q = realloc(b->p, cap);
    if (!q) return 0;
    b->p = q;
    b->cap = cap;
    return 1;
}

/* ---- XXH64 ---- */

#define P1 11400714785074694791ull
#define P2 14029467366897019727ull
#define P3 1609587929392839161ull
#define P4 9650029242287828579ull
#define P5 2870177450012600261ull

static inline uint64_t rotl64(uint64_t x, int r) { return (x << r) | (x >> (64 - r)); }
static inline uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = v << 8 | p[i];
    return v;
}
static inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint64_t xround(uint64_t acc, uint64_t in) { return rotl64(acc + in * P2, 31) * P1; }
static inline uint64_t xmerge(uint64_t acc, uint64_t v) { return (acc ^ xround(0, v)) * P1 + P4; }

static uint64_t xxh64(const uint8_t *p, size_t n) {
    const uint8_t *end = p + n;
    uint64_t h;
    if (n >= 32) {
        uint64_t v1 = P1 + P2, v2 = P2, v3 = 0, v4 = (uint64_t)0 - P1;
        const uint8_t *lim = end - 32;
        do {
            v1 = xround(v1, rd64(p));
            v2 = xround(v2, rd64(p + 8));
            v3 = xround(v3, rd64(p + 16));
            v4 = xround(v4, rd64(p + 24));
            p += 32;
        } while (p <= lim);
        h = rotl64(v1, 1) + rotl64(v2, 7) + rotl64(v3, 12) + rotl64(v4, 18);
        h = xmerge(h, v1);
        h = xmerge(h, v2);
        h = xmerge(h, v3);
        h = xmerge(h, v4);
    } else {
        h = P5;
    }
    h += (uint64_t)n;
    while (p + 8 <= end) {
        h ^= xround(0, rd64(p));
        h = rotl64(h, 27) * P1 + P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (uint64_t)rd32(p) * P1;
        h = rotl64(h, 23) * P2 + P3;
        p += 4;
    }
    while (p < end) {
        h ^= (*p++) * P5;
        h = rotl64(h, 11) * P1;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P3;
    h ^= h >> 32;
    return h;
}

/* ---- bit readers ---- */

/* Forward little-endian bit reader (FSE table descriptions). */
typedef struct {
    const uint8_t *p;
    size_t n;
    size_t bit;
} fbits;

static uint32_t fb_read(fbits *b, int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; i++, b->bit++) {
        size_t byte = b->bit >> 3;
        if (byte < b->n) v |= (uint32_t)((b->p[byte] >> (b->bit & 7)) & 1) << i;
    }
    return v;
}

static uint32_t fb_peek(fbits *b, int k) {
    size_t save = b->bit;
    uint32_t v = fb_read(b, k);
    b->bit = save;
    return v;
}

/* Backward bit reader (Huffman and sequence streams): the stream is read
 * from its last bit towards its first; pos counts the bits left. */
typedef struct {
    const uint8_t *p;
    size_t n;
    int64_t pos;
} bbits;

static int bb_init(bbits *b, const uint8_t *p, size_t n) {
    if (n == 0 || p[n - 1] == 0) return 0;
    b->p = p;
    b->n = n;
    int hb = 7;
    while (!((p[n - 1] >> hb) & 1)) hb--;
    b->pos = (int64_t)(n - 1) * 8 + hb;
    return 1;
}

/* The k bits below pos (k <= 32), as an integer with the first-read bit
 * highest; bits before the start of the stream read as 0. */
static inline uint64_t bb_peek(const bbits *b, int k) {
    int64_t lo = b->pos - k;
    uint64_t v = 0;
    int shift = 0;
    if (lo < 0) {
        shift = (int)-lo;
        lo = 0;
    }
    size_t byte = (size_t)(lo >> 3);
    uint64_t w = 0;
    for (int i = 0; i < 8 && byte + (size_t)i < b->n; i++) w |= (uint64_t)b->p[byte + (size_t)i] << (8 * i);
    v = w >> (lo & 7);
    int take = k - shift;
    v &= take >= 64 ? ~0ull : ((1ull << take) - 1);
    return v << shift;
}

static inline uint64_t bb_read(bbits *b, int k) {
    if (k == 0) return 0;
    uint64_t v = bb_peek(b, k);
    b->pos -= k;
    return v;
}

/* ---- FSE ---- */

typedef struct {
    uint16_t sym;
    uint8_t bits;
    uint16_t base;
} fse_entry;

typedef struct {
    int log;
    fse_entry t[512];
} fse_table;

static int highbit(uint32_t v) {
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

static int fse_build(fse_table *ft, const int16_t *norm, int nsym, int log) {
    int size = 1 << log;
    uint16_t next[256];
    int high = size - 1;
    ft->log = log;
    for (int s = 0; s < nsym; s++) {
        if (norm[s] == -1) {
            ft->t[high--].sym = (uint16_t)s;
            next[s] = 1;
        } else {
            next[s] = (uint16_t)(norm[s] > 0 ? norm[s] : 0);
        }
    }
    int pos = 0, step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
    for (int s = 0; s < nsym; s++) {
        for (int i = 0; i < norm[s]; i++) {
            ft->t[pos].sym = (uint16_t)s;
            do pos = (pos + step) & mask;
            while (pos > high);
        }
    }
    if (pos != 0) return 0;
    for (int u = 0; u < size; u++) {
        int s = ft->t[u].sym;
        uint32_t ns = next[s]++;
        int nb = log - highbit(ns);
        ft->t[u].bits = (uint8_t)nb;
        ft->t[u].base = (uint16_t)((ns << nb) - (uint32_t)size);
    }
    return 1;
}

/* Reads an FSE table description; returns the bytes used, 0 on error. */
static size_t fse_read(fse_table *ft, const uint8_t *p, size_t n, int max_sym, int max_log) {
    fbits b = {p, n, 0};
    int log = (int)fb_read(&b, 4) + 5;
    if (log > max_log) return 0;
    int16_t norm[256] = {0};
    int remaining = (1 << log) + 1, threshold = 1 << log, nb = log + 1, s = 0;
    while (remaining > 1 && s <= max_sym) {
        int max = (2 * threshold - 1) - remaining;
        int count;
        uint32_t v = fb_peek(&b, nb);
        if ((int)(v & (uint32_t)(threshold - 1)) < max) {
            count = (int)(v & (uint32_t)(threshold - 1));
            b.bit += (size_t)(nb - 1);
        } else {
            count = (int)(v & (uint32_t)(2 * threshold - 1));
            if (count >= threshold) count -= max;
            b.bit += (size_t)nb;
        }
        count--;
        remaining -= count < 0 ? -count : count;
        norm[s++] = (int16_t)count;
        if (count == 0) {
            for (;;) {
                int rep = (int)fb_read(&b, 2);
                for (int k = 0; k < rep && s <= max_sym; k++) norm[s++] = 0;
                if (rep < 3) break;
            }
        }
        while (remaining < threshold) {
            nb--;
            threshold >>= 1;
        }
    }
    if (remaining != 1 || s > max_sym + 1) return 0;
    size_t used = (b.bit + 7) >> 3;
    if (used > n) return 0;
    if (!fse_build(ft, norm, s, log)) return 0;
    return used;
}

static void fse_rle(fse_table *ft, int sym) {
    ft->log = 0;
    ft->t[0].sym = (uint16_t)sym;
    ft->t[0].bits = 0;
    ft->t[0].base = 0;
}

static const int16_t ll_default[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t ml_default[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t of_default[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};

static const uint32_t ll_base[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t ll_bits[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t ml_base[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t ml_bits[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

/* ---- Huffman (literals) ---- */

typedef struct {
    int max_bits;
    uint8_t sym[1 << 11];
    uint8_t bits[1 << 11];
} huf_table;

/* Builds the decoding table from weights (the last one implied). */
static int huf_from_weights(huf_table *h, uint8_t *w, int n) {
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        if (w[i] > 11) return 0;
        if (w[i]) total += 1u << (w[i] - 1);
    }
    if (total == 0) return 0;
    int max_bits = highbit(total) + 1;
    uint32_t left = (1u << max_bits) - total;
    if (left & (left - 1)) return 0; /* not a power of two */
    if (n >= 256) return 0;
    w[n++] = (uint8_t)(highbit(left) + 1);
    if (max_bits > 11) return 0;
    h->max_bits = max_bits;
    uint32_t rank[13] = {0};
    for (int i = 0; i < n; i++) rank[w[i]]++;
    uint32_t start[13], next = 0;
    for (int k = 1; k <= max_bits; k++) {
        start[k] = next;
        next += rank[k] << (k - 1);
    }
    if (next != (1u << max_bits)) return 0;
    for (int s = 0; s < n; s++) {
        int k = w[s];
        if (!k) continue;
        uint32_t len = 1u << (k - 1);
        for (uint32_t u = start[k]; u < start[k] + len; u++) {
            h->sym[u] = (uint8_t)s;
            h->bits[u] = (uint8_t)(max_bits + 1 - k);
        }
        start[k] += len;
    }
    return 1;
}

/* Reads a Huffman tree description; returns the bytes used, 0 on error. */
static size_t huf_read(huf_table *h, const uint8_t *p, size_t n) {
    if (n < 1) return 0;
    uint8_t w[256] = {0};
    int nw = 0;
    int hb = p[0];
    size_t used;
    if (hb >= 128) {
        nw = hb - 127;
        used = 1 + (size_t)(nw + 1) / 2;
        if (used > n) return 0;
        for (int i = 0; i < nw; i++) {
            uint8_t byte = p[1 + i / 2];
            w[i] = (uint8_t)(i % 2 == 0 ? byte >> 4 : byte & 15);
        }
    } else {
        used = 1 + (size_t)hb;
        if (used > n || hb == 0) return 0;
        fse_table ft;
        size_t tl = fse_read(&ft, p + 1, (size_t)hb, 255, 6);
        if (!tl) return 0;
        bbits b;
        if (!bb_init(&b, p + 1 + tl, (size_t)hb - tl)) return 0;
        uint32_t s1 = (uint32_t)bb_read(&b, ft.log), s2 = (uint32_t)bb_read(&b, ft.log);
        for (;;) {
            if (nw >= 255) return 0;
            w[nw++] = (uint8_t)ft.t[s1].sym;
            if (b.pos < ft.t[s1].bits) {
                w[nw++] = (uint8_t)ft.t[s2].sym;
                break;
            }
            s1 = ft.t[s1].base + (uint32_t)bb_read(&b, ft.t[s1].bits);
            if (nw >= 255) return 0;
            w[nw++] = (uint8_t)ft.t[s2].sym;
            if (b.pos < ft.t[s2].bits) {
                w[nw++] = (uint8_t)ft.t[s1].sym;
                break;
            }
            s2 = ft.t[s2].base + (uint32_t)bb_read(&b, ft.t[s2].bits);
        }
    }
    if (!huf_from_weights(h, w, nw)) return 0;
    return used;
}

static int huf_stream(const huf_table *h, const uint8_t *p, size_t n, uint8_t *out, size_t count) {
    bbits b;
    if (!bb_init(&b, p, n)) return 0;
    int mb = h->max_bits;
    for (size_t i = 0; i < count; i++) {
        uint32_t idx = (uint32_t)bb_peek(&b, mb);
        out[i] = h->sym[idx];
        b.pos -= h->bits[idx];
        if (b.pos < 0) return 0;
    }
    return b.pos == 0;
}

/* ---- frames ---- */

typedef struct {
    huf_table huf;
    int have_huf;
    fse_table ll, of, ml;
    int have_ll, have_of, have_ml;
    uint32_t rep[3];
    uint8_t *lits;
    size_t lits_cap;
} zctx;

static const char *seq_table(zctx *z, int mode, fse_table *ft, int *have, const int16_t *def, int ndef, int deflog, int maxsym, int maxlog, const uint8_t **pp, const uint8_t *end) {
    if (mode == 0) {
        fse_build(ft, def, ndef, deflog);
        *have = 1;
    } else if (mode == 1) {
        if (*pp >= end) return "zstd: truncated sequence table";
        if (**pp > maxsym) return "zstd: invalid RLE sequence symbol";
        fse_rle(ft, **pp);
        (*pp)++;
        *have = 1;
    } else if (mode == 2) {
        size_t u = fse_read(ft, *pp, (size_t)(end - *pp), maxsym, maxlog);
        if (!u) return "zstd: invalid sequence table";
        *pp += u;
        *have = 1;
    } else if (!*have) {
        return "zstd: repeat of a missing sequence table";
    }
    (void)z;
    return NULL;
}

static const char *decode_block(zctx *z, const uint8_t *p, size_t n, zbuf *out, size_t frame_start, size_t window) {
    const uint8_t *end = p + n;
    /* literals */
    if (n < 1) return "zstd: truncated block";
    int ltype = p[0] & 3, sf = (p[0] >> 2) & 3;
    size_t regen, csize = 0, hl;
    if (ltype <= 1) {
        if (sf == 0 || sf == 2) {
            regen = p[0] >> 3;
            hl = 1;
        } else if (sf == 1) {
            if (n < 2) return "zstd: truncated literals header";
            regen = (size_t)(p[0] >> 4) + ((size_t)p[1] << 4);
            hl = 2;
        } else {
            if (n < 3) return "zstd: truncated literals header";
            regen = (size_t)(p[0] >> 4) + ((size_t)p[1] << 4) + ((size_t)p[2] << 12);
            hl = 3;
        }
    } else {
        if (sf <= 1) {
            if (n < 3) return "zstd: truncated literals header";
            uint32_t h = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            regen = (h >> 4) & 0x3FF;
            csize = (h >> 14) & 0x3FF;
            hl = 3;
        } else if (sf == 2) {
            if (n < 4) return "zstd: truncated literals header";
            uint32_t h = rd32(p);
            regen = (h >> 4) & 0x3FFF;
            csize = (h >> 18) & 0x3FFF;
            hl = 4;
        } else {
            if (n < 5) return "zstd: truncated literals header";
            uint64_t h = (uint64_t)rd32(p) | (uint64_t)p[4] << 32;
            regen = (size_t)((h >> 4) & 0x3FFFF);
            csize = (size_t)((h >> 22) & 0x3FFFF);
            hl = 5;
        }
    }
    if (regen > 131072) return "zstd: literals too large";
    if (z->lits_cap < regen + 8) {
        z->lits_cap = regen + 8 > 131080 ? regen + 8 : 131080;
        uint8_t *q = realloc(z->lits, z->lits_cap);
        if (!q) return "zstd: out of memory";
        z->lits = q;
    }
    const uint8_t *q = p + hl;
    if (ltype == 0) {
        if ((size_t)(end - q) < regen) return "zstd: truncated literals";
        memcpy(z->lits, q, regen);
        q += regen;
    } else if (ltype == 1) {
        if (q >= end) return "zstd: truncated literals";
        memset(z->lits, *q, regen);
        q++;
    } else {
        if ((size_t)(end - q) < csize) return "zstd: truncated literals";
        const uint8_t *lp = q, *lend = q + csize;
        if (ltype == 2) {
            size_t u = huf_read(&z->huf, lp, csize);
            if (!u) return "zstd: invalid Huffman table";
            z->have_huf = 1;
            lp += u;
        } else if (!z->have_huf) {
            return "zstd: treeless literals without a previous table";
        }
        if (sf == 0) {
            if (!huf_stream(&z->huf, lp, (size_t)(lend - lp), z->lits, regen)) return "zstd: corrupt literals stream";
        } else {
            if (lend - lp < 6) return "zstd: truncated jump table";
            size_t s1 = (size_t)lp[0] | (size_t)lp[1] << 8, s2 = (size_t)lp[2] | (size_t)lp[3] << 8, s3 = (size_t)lp[4] | (size_t)lp[5] << 8;
            lp += 6;
            if (s1 + s2 + s3 > (size_t)(lend - lp)) return "zstd: invalid jump table";
            size_t s4 = (size_t)(lend - lp) - s1 - s2 - s3;
            size_t seg = (regen + 3) / 4;
            if (seg * 3 > regen) return "zstd: invalid literals size";
            if (!huf_stream(&z->huf, lp, s1, z->lits, seg) || !huf_stream(&z->huf, lp + s1, s2, z->lits + seg, seg) ||
                !huf_stream(&z->huf, lp + s1 + s2, s3, z->lits + 2 * seg, seg) ||
                !huf_stream(&z->huf, lp + s1 + s2 + s3, s4, z->lits + 3 * seg, regen - 3 * seg))
                return "zstd: corrupt literals stream";
        }
        q = lend;
    }
    /* sequences */
    if (q >= end) return "zstd: truncated sequences section";
    size_t nseq;
    if (*q < 128) {
        nseq = *q++;
    } else if (*q < 255) {
        if (end - q < 2) return "zstd: truncated sequences header";
        nseq = ((size_t)(q[0] - 128) << 8) + q[1];
        q += 2;
    } else {
        if (end - q < 3) return "zstd: truncated sequences header";
        nseq = (size_t)q[1] + ((size_t)q[2] << 8) + 0x7F00;
        q += 3;
    }
    size_t lit_pos = 0;
    if (nseq > 0) {
        if (q >= end) return "zstd: truncated sequences header";
        int modes = *q++;
        if (modes & 3) return "zstd: reserved bits set in sequence modes";
        const char *e;
        if ((e = seq_table(z, modes >> 6, &z->ll, &z->have_ll, ll_default, 36, 6, 35, 9, &q, end))) return e;
        if ((e = seq_table(z, (modes >> 4) & 3, &z->of, &z->have_of, of_default, 29, 5, 31, 8, &q, end))) return e;
        if ((e = seq_table(z, (modes >> 2) & 3, &z->ml, &z->have_ml, ml_default, 53, 6, 52, 9, &q, end))) return e;
        bbits b;
        if (!bb_init(&b, q, (size_t)(end - q))) return "zstd: corrupt sequences stream";
        uint32_t sl = (uint32_t)bb_read(&b, z->ll.log), so = (uint32_t)bb_read(&b, z->of.log), sm = (uint32_t)bb_read(&b, z->ml.log);
        for (size_t i = 0; i < nseq; i++) {
            int llc = z->ll.t[sl].sym, ofc = z->of.t[so].sym, mlc = z->ml.t[sm].sym;
            if (llc > 35 || mlc > 52 || ofc > 31) return "zstd: invalid sequence code";
            uint64_t ofv = ((uint64_t)1 << ofc) + bb_read(&b, ofc);
            uint64_t ml = ml_base[mlc] + bb_read(&b, ml_bits[mlc]);
            uint64_t ll = ll_base[llc] + bb_read(&b, ll_bits[llc]);
            if (b.pos < 0) return "zstd: corrupt sequences stream";
            uint64_t off;
            if (ofv > 3) {
                off = ofv - 3;
                z->rep[2] = z->rep[1];
                z->rep[1] = z->rep[0];
                z->rep[0] = (uint32_t)off;
            } else {
                int idx = (int)ofv - 1 + (ll == 0 ? 1 : 0);
                if (idx == 0) {
                    off = z->rep[0];
                } else if (idx == 1) {
                    off = z->rep[1];
                    z->rep[1] = z->rep[0];
                    z->rep[0] = (uint32_t)off;
                } else if (idx == 2) {
                    off = z->rep[2];
                    z->rep[2] = z->rep[1];
                    z->rep[1] = z->rep[0];
                    z->rep[0] = (uint32_t)off;
                } else {
                    off = (uint64_t)z->rep[0] - 1;
                    if (off == 0) return "zstd: invalid repeat offset";
                    z->rep[2] = z->rep[1];
                    z->rep[1] = z->rep[0];
                    z->rep[0] = (uint32_t)off;
                }
            }
            if (lit_pos + ll > regen) return "zstd: literals overrun";
            if (!zb_grow(out, (size_t)(ll + ml))) return "zstd: out of memory";
            memcpy(out->p + out->n, z->lits + lit_pos, (size_t)ll);
            out->n += (size_t)ll;
            lit_pos += (size_t)ll;
            if (off > out->n - frame_start || off > window) return "zstd: offset beyond the window";
            uint8_t *d = out->p + out->n;
            const uint8_t *src = d - off;
            if (off >= ml) {
                memcpy(d, src, (size_t)ml);
            } else {
                for (uint64_t k = 0; k < ml; k++) d[k] = src[k];
            }
            out->n += (size_t)ml;
            if (i + 1 < nseq) {
                sl = z->ll.t[sl].base + (uint32_t)bb_read(&b, z->ll.t[sl].bits);
                sm = z->ml.t[sm].base + (uint32_t)bb_read(&b, z->ml.t[sm].bits);
                so = z->of.t[so].base + (uint32_t)bb_read(&b, z->of.t[so].bits);
                if (b.pos < 0) return "zstd: corrupt sequences stream";
            }
        }
        if (b.pos != 0) return "zstd: corrupt sequences stream";
    } else if (q != end) {
        return "zstd: unexpected data after the literals";
    }
    if (!zb_grow(out, regen - lit_pos)) return "zstd: out of memory";
    memcpy(out->p + out->n, z->lits + lit_pos, regen - lit_pos);
    out->n += regen - lit_pos;
    return NULL;
}

/* Length of the frame at p (magic included), 0 if p ends inside it; -1
 * (as SIZE_MAX) if the data is not a frame. When out is not NULL the frame
 * is decoded into it. */
static const char *frame(const uint8_t *p, size_t n, size_t *len, zbuf *out) {
    *len = 0;
    if (n < 4) return NULL;
    uint32_t magic = rd32(p);
    if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {
        if (n < 8) return NULL;
        size_t sz = rd32(p + 4);
        if (n - 8 < sz) return NULL;
        *len = 8 + sz;
        return NULL;
    }
    if (magic != 0xFD2FB528u) return "zstd: invalid magic number";
    if (n < 5) return NULL;
    int fhd = p[4];
    int fcs_flag = fhd >> 6, single = (fhd >> 5) & 1, checksum = (fhd >> 2) & 1, did_flag = fhd & 3;
    if (fhd & 8) return "zstd: reserved bit set in the frame header";
    size_t i = 5;
    uint64_t window = 0;
    if (!single) {
        if (n < i + 1) return NULL;
        int wd = p[i++];
        int exp = wd >> 3, mant = wd & 7;
        uint64_t base = (uint64_t)1 << (10 + exp);
        window = base + (base / 8) * (uint64_t)mant;
    }
    static const int did_sizes[4] = {0, 1, 2, 4};
    size_t ds = (size_t)did_sizes[did_flag];
    if (n < i + ds) return NULL;
    uint32_t did = 0;
    for (size_t k = 0; k < ds; k++) did |= (uint32_t)p[i + k] << (8 * k);
    i += ds;
    if (did != 0) return "zstd: dictionaries are not supported";
    static const int fcs_sizes[4] = {0, 2, 4, 8};
    size_t fs = (size_t)fcs_sizes[fcs_flag];
    if (fcs_flag == 0 && single) fs = 1;
    if (n < i + fs) return NULL;
    uint64_t fcs = 0;
    int have_fcs = fs > 0;
    for (size_t k = 0; k < fs; k++) fcs |= (uint64_t)p[i + k] << (8 * k);
    if (fs == 2) fcs += 256;
    i += fs;
    if (single) window = fcs;
    if (window > ((uint64_t)1 << 31)) return "zstd: window too large";
    size_t block_max = window < 131072 ? (size_t)window : 131072;
    zctx z;
    memset(&z, 0, sizeof z);
    z.rep[0] = 1;
    z.rep[1] = 4;
    z.rep[2] = 8;
    size_t start = out ? out->n : 0;
    const char *err = NULL;
    for (;;) {
        if (n < i + 3) goto incomplete;
        uint32_t bh = (uint32_t)p[i] | (uint32_t)p[i + 1] << 8 | (uint32_t)p[i + 2] << 16;
        i += 3;
        int last = bh & 1, type = (bh >> 1) & 3;
        size_t bsize = bh >> 3;
        if (type == 3) {
            err = "zstd: reserved block type";
            goto done;
        }
        size_t csize = type == 1 ? 1 : bsize;
        if (n < i + csize) goto incomplete;
        if (bsize > block_max && type != 1) {
            err = "zstd: block too large";
            goto done;
        }
        if (out) {
            if (type == 0) {
                if (!zb_grow(out, bsize)) { err = "zstd: out of memory"; goto done; }
                memcpy(out->p + out->n, p + i, bsize);
                out->n += bsize;
            } else if (type == 1) {
                if (!zb_grow(out, bsize)) { err = "zstd: out of memory"; goto done; }
                memset(out->p + out->n, p[i], bsize);
                out->n += bsize;
            } else {
                err = decode_block(&z, p + i, csize, out, start, window ? (size_t)window : SIZE_MAX);
                if (err) goto done;
            }
        }
        i += csize;
        if (last) break;
    }
    if (checksum) {
        if (n < i + 4) goto incomplete;
        if (out) {
            uint32_t want = rd32(p + i);
            uint32_t got = (uint32_t)xxh64(out->p + start, out->n - start);
            if (want != got) {
                err = "zstd: checksum mismatch";
                goto done;
            }
        }
        i += 4;
    }
    if (out && have_fcs && out->n - start != fcs) {
        err = "zstd: frame content size mismatch";
        goto done;
    }
    *len = i;
done:
    free(z.lits);
    return err;
incomplete:
    free(z.lits);
    *len = 0;
    return out ? "zstd: unexpected end of data" : NULL;
}

/* Decodes all frames of data (skippable frames are skipped). */
void zell_compress_rt_zstd_decode(const zell_slice *data, zell_slice *out, zell_error **err) {
    if (err) *err = NULL;
    memset(out, 0, sizeof *out);
    const uint8_t *p = data && data->len ? data->data : NULL;
    size_t n = data ? (size_t)data->len : 0, i = 0;
    zbuf ob = {0};
    if (n == 0) {
        if (err) *err = zell_error_new_cstr("zstd: unexpected end of data");
        return;
    }
    while (i < n) {
        size_t len;
        const char *e = frame(p + i, n - i, &len, &ob);
        if (!e && len == 0) e = "zstd: unexpected end of data";
        if (e) {
            free(ob.p);
            if (err) *err = zell_error_new_cstr(e);
            return;
        }
        i += len;
    }
    if (ob.n) {
        zell_slice_new(out, 1, (int64_t)ob.n, (int64_t)ob.n);
        memcpy(out->data, ob.p, ob.n);
    }
    free(ob.p);
}

/* The length of the complete frame at the start of data, 0 if data ends
 * inside it, -1 if data does not start with a frame. */
int64_t zell_compress_rt_zstd_frame_len(const zell_slice *data) {
    const uint8_t *p = data && data->len ? data->data : NULL;
    size_t n = data ? (size_t)data->len : 0, len;
    if (n < 4) return 0;
    const char *e = frame(p, n, &len, NULL);
    if (e) return -1;
    return (int64_t)len;
}

/* XXH64 of data with seed 0 (exposed for tests and the Zell side). */
int64_t zell_compress_rt_xxh64(const zell_slice *data) {
    const uint8_t *p = data && data->len ? data->data : (const uint8_t *)"";
    return (int64_t)xxh64(p, data ? (size_t)data->len : 0);
}

/* XXH64 (seed 0) for the encoder (std_zstd_enc.c). */
uint64_t zell__zstd_xxh64(const uint8_t *p, size_t n);
uint64_t zell__zstd_xxh64(const uint8_t *p, size_t n) { return xxh64(p, n); }
