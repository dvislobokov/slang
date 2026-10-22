/* std_zstd_enc.c - Zstandard encoder (RFC 8878) for compress.zstd
 * Compress (runtime/ABI.md 23). Whole inputs are compressed into one
 * frame: single-segment up to 8 MiB (window = content), otherwise an
 * 8 MiB window. Blocks of up to 128 KiB are emitted as Raw, RLE or
 * Compressed blocks, whichever is smallest. A compressed block has
 * Huffman-coded literals (one or four streams; weights stored directly or
 * FSE-compressed) or raw/RLE literals, and sequences whose codes use the
 * predefined distributions (small blocks), RLE or block-specific FSE
 * tables. Matches come from a hash-chain match finder over 4-byte
 * prefixes plus the last repeat offset; the level sets the chain depth
 * (1: one probe, greedy) and enables lazy matching from level 5. Repeat
 * offsets are coded after literals (Offset_Value 1-3). The content
 * checksum (XXH64) is optional. */
#include "rt_internal.h"

#include <stdlib.h>
#include <string.h>

uint64_t zell__zstd_xxh64(const uint8_t *p, size_t n);

typedef struct {
    uint8_t *p;
    size_t n, cap;
    int oom;
} ebuf;

static int eb_grow(ebuf *b, size_t need) {
    if (b->oom) return 0;
    if (b->n + need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need) cap *= 2;
    uint8_t *q = realloc(b->p, cap);
    if (!q) {
        b->oom = 1;
        return 0;
    }
    b->p = q;
    b->cap = cap;
    return 1;
}

static void eb_byte(ebuf *b, uint8_t v) {
    if (eb_grow(b, 1)) b->p[b->n++] = v;
}

static void eb_bytes(ebuf *b, const uint8_t *p, size_t n) {
    if (n && eb_grow(b, n)) {
        memcpy(b->p + b->n, p, n);
        b->n += n;
    }
}

static void eb_le(ebuf *b, uint64_t v, int n) {
    for (int i = 0; i < n; i++) eb_byte(b, (uint8_t)(v >> (8 * i)));
}

/* ---- bit writer (streams read backwards by the decoder) ---- */

typedef struct {
    uint64_t acc;
    int nb;
    ebuf *out;
} bitw;

static void bw_add(bitw *w, uint64_t v, int n) {
    if (n == 0) return;
    w->acc |= (v & (n >= 64 ? ~0ull : ((1ull << n) - 1))) << w->nb;
    w->nb += n;
    while (w->nb >= 8) {
        eb_byte(w->out, (uint8_t)w->acc);
        w->acc >>= 8;
        w->nb -= 8;
    }
}

static void bw_close(bitw *w) {
    bw_add(w, 1, 1);
    if (w->nb > 0) eb_byte(w->out, (uint8_t)w->acc);
    w->acc = 0;
    w->nb = 0;
}

/* Forward bit writer for FSE table descriptions (read forward). */
static void fw_add(ebuf *b, size_t *bit, uint32_t v, int n) {
    for (int i = 0; i < n; i++, (*bit)++) {
        size_t byte = *bit >> 3;
        while (b->n <= byte) eb_byte(b, 0);
        if (b->oom) return;
        if ((v >> i) & 1) b->p[byte] |= (uint8_t)(1u << (*bit & 7));
    }
}

/* ---- FSE encoding tables ---- */

static int hb32(uint32_t v) {
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

typedef struct {
    int log;
    int rle;            /* RLE mode: one symbol, no state bits */
    uint16_t state[512];
    int32_t dnb[256];
    int32_t dfs[256];
} fse_ctab;

static void fse_cbuild(fse_ctab *c, const int16_t *norm, int nsym, int log) {
    int size = 1 << log, high = size - 1;
    uint8_t sym[512];
    uint32_t cumul[257];
    c->log = log;
    c->rle = 0;
    cumul[0] = 0;
    for (int s = 0; s < nsym; s++) {
        if (norm[s] == -1) {
            cumul[s + 1] = cumul[s] + 1;
            sym[high--] = (uint8_t)s;
        } else {
            cumul[s + 1] = cumul[s] + (uint32_t)(norm[s] > 0 ? norm[s] : 0);
        }
    }
    int pos = 0, step = (size >> 1) + (size >> 3) + 3, mask = size - 1;
    for (int s = 0; s < nsym; s++) {
        for (int i = 0; i < norm[s]; i++) {
            sym[pos] = (uint8_t)s;
            do pos = (pos + step) & mask;
            while (pos > high);
        }
    }
    uint32_t next[257];
    memcpy(next, cumul, sizeof next);
    for (int u = 0; u < size; u++) c->state[next[sym[u]]++] = (uint16_t)(size + u);
    int total = 0;
    for (int s = 0; s < nsym; s++) {
        int n = norm[s];
        if (n == 0) {
            c->dnb[s] = ((log + 1) << 16) - size;
            c->dfs[s] = 0;
        } else if (n == -1 || n == 1) {
            c->dnb[s] = (log << 16) - size;
            c->dfs[s] = total - 1;
            total++;
        } else {
            int maxOut = log - hb32((uint32_t)n - 1);
            int minPlus = n << maxOut;
            c->dnb[s] = (maxOut << 16) - minPlus;
            c->dfs[s] = total - n;
            total += n;
        }
    }
}

static uint32_t fse_init(const fse_ctab *c, int s) {
    if (c->rle) return 0;
    uint32_t nbOut = (uint32_t)(c->dnb[s] + (1 << 15)) >> 16;
    uint32_t v = (nbOut << 16) - (uint32_t)c->dnb[s];
    return c->state[(v >> nbOut) + (uint32_t)c->dfs[s]];
}

static void fse_enc(bitw *w, const fse_ctab *c, uint32_t *st, int s) {
    if (c->rle) return;
    uint32_t nbOut = (*st + (uint32_t)c->dnb[s]) >> 16;
    bw_add(w, *st, (int)nbOut);
    *st = c->state[(*st >> nbOut) + (uint32_t)c->dfs[s]];
}

static void fse_flush(bitw *w, const fse_ctab *c, uint32_t st) {
    if (!c->rle) bw_add(w, st, c->log);
}

/* Normalizes counts (total > 0) to sum 1 << log; every present symbol gets
 * at least 1. */
static void fse_normalize(int16_t *norm, const uint32_t *count, int nsym, int log, uint32_t total) {
    int size = 1 << log, sum = 0, big = -1;
    for (int s = 0; s < nsym; s++) {
        if (!count[s]) {
            norm[s] = 0;
            continue;
        }
        int v = (int)(((uint64_t)count[s] * (uint64_t)size + total / 2) / total);
        if (v < 1) v = 1;
        norm[s] = (int16_t)v;
        sum += v;
        if (big < 0 || norm[s] > norm[big]) big = s;
    }
    /* fix the sum: take from / give to the largest entries */
    while (sum > size) {
        int b = -1;
        for (int s = 0; s < nsym; s++)
            if (norm[s] > 1 && (b < 0 || norm[s] > norm[b])) b = s;
        norm[b]--;
        sum--;
    }
    if (sum < size) norm[big] = (int16_t)(norm[big] + size - sum);
}

/* Writes an FSE table description (RFC 8878 4.1.1). */
static void fse_write_ncount(ebuf *b, const int16_t *norm, int nsym, int log) {
    size_t start = b->n, bit = 0;
    (void)start;
    ebuf t = { 0 };
    fw_add(&t, &bit, (uint32_t)(log - 5), 4);
    int remaining = (1 << log) + 1, threshold = 1 << log, nb = log + 1;
    int s = 0;
    while (remaining > 1 && s < nsym) {
        int count = norm[s];
        int value = count + 1;
        int max = (2 * threshold - 1) - remaining;
        if (value < max) {
            fw_add(&t, &bit, (uint32_t)value, nb - 1);
        } else if (value < threshold) {
            fw_add(&t, &bit, (uint32_t)value, nb);
        } else {
            fw_add(&t, &bit, (uint32_t)(value + max), nb);
        }
        remaining -= count < 0 ? -count : count;
        s++;
        if (count == 0) {
            int zeros = 0;
            while (s + zeros < nsym && norm[s + zeros] == 0) zeros++;
            s += zeros;
            for (;;) {
                int rep = zeros >= 3 ? 3 : zeros;
                fw_add(&t, &bit, (uint32_t)rep, 2);
                zeros -= rep;
                if (rep < 3) break;
            }
        }
        while (remaining < threshold) {
            nb--;
            threshold >>= 1;
        }
    }
    eb_bytes(b, t.p, (bit + 7) >> 3);
    free(t.p);
}

/* ---- sequence codes ---- */

static const int16_t ll_def[36] = {4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1};
static const int16_t ml_def[53] = {1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1, -1, -1};
static const int16_t of_def[29] = {1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1, -1};
static const uint32_t llb[36] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const uint8_t llx[36] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
static const uint32_t mlb[53] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051, 4099, 8195, 16387, 32771, 65539};
static const uint8_t mlx[53] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

static int ll_code(uint32_t v) {
    if (v < 16) return (int)v;
    int c = 35;
    while (llb[c] > v) c--;
    return c;
}

static int ml_code(uint32_t v) {
    if (v < 35) return (int)v - 3;
    int c = 52;
    while (mlb[c] > v) c--;
    return c;
}

typedef struct {
    uint32_t ll, ml, ofv;   /* ofv: the coded Offset_Value (1-3 repeat, else offset + 3) */
} seq;

/* ---- Huffman literals ---- */

/* Code lengths (<= 11) for the frequencies; returns the max length. */
static int huf_lengths(const uint32_t *freq0, uint8_t *len) {
    uint32_t freq[256];
    memcpy(freq, freq0, sizeof freq);
    for (;;) {
        /* package by sorting leaves and merging with a queue (Huffman) */
        int n = 0, leaf[256];
        for (int s = 0; s < 256; s++)
            if (freq[s]) leaf[n++] = s;
        for (int i = 1; i < n; i++) {
            int x = leaf[i], j = i - 1;
            while (j >= 0 && (freq[leaf[j]] > freq[x] || (freq[leaf[j]] == freq[x] && leaf[j] > x))) {
                leaf[j + 1] = leaf[j];
                j--;
            }
            leaf[j + 1] = x;
        }
        uint64_t w[512];
        int parent[512];
        for (int i = 0; i < n; i++) {
            w[i] = freq[leaf[i]];
            parent[i] = -1;
        }
        int nodes = n, li = 0, qi = n;
        while ((n - li) + (nodes - qi) > 1) {
            int pick[2];
            for (int k = 0; k < 2; k++) {
                if (li < n && (qi >= nodes || w[li] <= w[qi])) pick[k] = li++;
                else pick[k] = qi++;
            }
            w[nodes] = w[pick[0]] + w[pick[1]];
            parent[nodes] = -1;
            parent[pick[0]] = nodes;
            parent[pick[1]] = nodes;
            nodes++;
        }
        int maxl = 0;
        memset(len, 0, 256);
        for (int i = 0; i < n; i++) {
            int d = 0;
            for (int x = i; parent[x] >= 0; x = parent[x]) d++;
            len[leaf[i]] = (uint8_t)d;
            if (d > maxl) maxl = d;
        }
        if (maxl <= 11) return maxl;
        for (int s = 0; s < 256; s++)
            if (freq[s]) freq[s] = (freq[s] >> 1) | 1;
    }
}

typedef struct {
    int maxBits;
    uint16_t code[256];
    uint8_t nb[256];
    uint8_t w[256];
    int last;   /* highest present symbol (its weight is implied) */
} huf_ctab;

static void huf_build(huf_ctab *h, const uint8_t *len) {
    int maxl = 0;
    h->last = 0;
    for (int s = 0; s < 256; s++) {
        if (len[s] > maxl) maxl = len[s];
        if (len[s]) h->last = s;
    }
    h->maxBits = maxl;
    uint32_t rank[13] = { 0 };
    for (int s = 0; s < 256; s++) {
        h->w[s] = len[s] ? (uint8_t)(maxl + 1 - len[s]) : 0;
        rank[h->w[s]]++;
    }
    uint32_t start[13], next = 0;
    for (int k = 1; k <= maxl; k++) {
        start[k] = next;
        next += rank[k] << (k - 1);
    }
    for (int s = 0; s < 256; s++) {
        int k = h->w[s];
        h->nb[s] = len[s];
        if (!k) continue;
        h->code[s] = (uint16_t)(start[k] >> (k - 1));
        start[k] += 1u << (k - 1);
    }
}

/* One Huffman stream of lits[0..n) (written last-to-first). */
static void huf_stream_enc(ebuf *out, const huf_ctab *h, const uint8_t *lits, size_t n) {
    bitw w = { 0, 0, out };
    for (size_t i = n; i > 0; i--) bw_add(&w, h->code[lits[i - 1]], h->nb[lits[i - 1]]);
    bw_close(&w);
}

/* Huffman tree description: direct 4-bit weights when the last symbol is
 * < 129, else FSE-compressed weights; 0 if it would not fit. */
static int huf_write_tree(ebuf *out, const huf_ctab *h) {
    int nw = h->last;   /* weights of symbols 0..last-1 */
    if (nw <= 128) {
        eb_byte(out, (uint8_t)(127 + nw));
        for (int i = 0; i < nw; i += 2) eb_byte(out, (uint8_t)(h->w[i] << 4 | (i + 1 < nw ? h->w[i + 1] : 0)));
        return 1;
    }
    uint32_t count[13] = { 0 };
    int maxw = 0;
    for (int i = 0; i < nw; i++) {
        count[h->w[i]]++;
        if (h->w[i] > maxw) maxw = h->w[i];
    }
    int16_t norm[13];
    int log = 6;
    fse_normalize(norm, count, maxw + 1, log, (uint32_t)nw);
    ebuf t = { 0 };
    fse_write_ncount(&t, norm, maxw + 1, log);
    fse_ctab ct;
    fse_cbuild(&ct, norm, maxw + 1, log);
    bitw w = { 0, 0, &t };
    const uint8_t *ip = h->w + nw;
    uint32_t s1, s2;
    size_t left = (size_t)nw;
    if (left & 1) {
        s1 = fse_init(&ct, *--ip);
        s2 = fse_init(&ct, *--ip);
        fse_enc(&w, &ct, &s1, *--ip);
        left -= 3;
    } else {
        s2 = fse_init(&ct, *--ip);
        s1 = fse_init(&ct, *--ip);
        left -= 2;
    }
    while (left > 0) {
        fse_enc(&w, &ct, &s2, *--ip);
        fse_enc(&w, &ct, &s1, *--ip);
        left -= 2;
    }
    fse_flush(&w, &ct, s2);
    fse_flush(&w, &ct, s1);
    bw_close(&w);
    int ok = !t.oom && t.n < 128;
    if (ok) {
        eb_byte(out, (uint8_t)t.n);
        eb_bytes(out, t.p, t.n);
    }
    free(t.p);
    return ok;
}

/* Literals section; returns 0 on failure (out of memory). */
static void write_literals(ebuf *out, const uint8_t *lits, size_t n) {
    uint32_t freq[256] = { 0 };
    int distinct = 0;
    for (size_t i = 0; i < n; i++)
        if (!freq[lits[i]]++) distinct++;
    if (n > 0 && distinct == 1) {
        /* RLE literals */
        if (n < 32) {
            eb_byte(out, (uint8_t)(1 | (n << 3)));
        } else if (n < 4096) {
            eb_byte(out, (uint8_t)(1 | 4 | ((n & 15) << 4)));
            eb_byte(out, (uint8_t)(n >> 4));
        } else {
            eb_byte(out, (uint8_t)(1 | 12 | ((n & 15) << 4)));
            eb_byte(out, (uint8_t)(n >> 4));
            eb_byte(out, (uint8_t)(n >> 12));
        }
        eb_byte(out, lits[0]);
        return;
    }
    if (n >= 64) {
        uint8_t len[256];
        huf_lengths(freq, len);
        huf_ctab h;
        huf_build(&h, len);
        ebuf body = { 0 };
        if (huf_write_tree(&body, &h)) {
            int four = n > 1023;
            if (!four) {
                huf_stream_enc(&body, &h, lits, n);
                if (body.n > 1023) four = 1;
            }
            if (four) {
                /* rebuild: tree + jump table + 4 streams */
                body.n = 0;
                huf_write_tree(&body, &h);
                size_t seg = (n + 3) / 4;
                size_t jt = body.n;
                eb_le(&body, 0, 6);
                size_t sizes[4], at = body.n;
                for (int k = 0; k < 4; k++) {
                    size_t lo = seg * (size_t)k, hi = k == 3 ? n : seg * (size_t)(k + 1);
                    huf_stream_enc(&body, &h, lits + lo, hi - lo);
                    sizes[k] = body.n - at;
                    at = body.n;
                }
                if (!body.oom)
                    for (int k = 0; k < 3; k++) {
                        body.p[jt + 2 * k] = (uint8_t)sizes[k];
                        body.p[jt + 2 * k + 1] = (uint8_t)(sizes[k] >> 8);
                    }
            }
            size_t cs = body.n;
            if (!body.oom && cs < n) {
                if (!four) {
                    uint32_t hd = 2 | (0 << 2) | ((uint32_t)n << 4) | ((uint32_t)cs << 14);
                    eb_le(out, hd, 3);
                } else if (n < 16384 && cs < 16384) {
                    uint32_t hd = 2 | (2 << 2) | ((uint32_t)n << 4) | ((uint32_t)cs << 18);
                    eb_le(out, hd, 4);
                } else {
                    uint64_t hd = 2 | (3 << 2) | ((uint64_t)n << 4) | ((uint64_t)cs << 22);
                    eb_le(out, hd, 5);
                }
                eb_bytes(out, body.p, cs);
                free(body.p);
                return;
            }
        }
        free(body.p);
    }
    /* raw literals */
    if (n < 32) {
        eb_byte(out, (uint8_t)(n << 3));
    } else if (n < 4096) {
        eb_byte(out, (uint8_t)(4 | ((n & 15) << 4)));
        eb_byte(out, (uint8_t)(n >> 4));
    } else {
        eb_byte(out, (uint8_t)(12 | ((n & 15) << 4)));
        eb_byte(out, (uint8_t)(n >> 4));
        eb_byte(out, (uint8_t)(n >> 12));
    }
    eb_bytes(out, lits, n);
}

static int seq_codes(const seq *q, int which) {
    if (which == 0) return ll_code(q->ll);
    if (which == 1) return hb32(q->ofv);
    return ml_code(q->ml);
}

/* Chooses the table of one sequence field: 0 predefined, 1 RLE, 2 FSE
 * compressed (description written to out). */
static int seq_table(ebuf *out, fse_ctab *c, const seq *sq, size_t ns, int which) {
    static const int16_t *defs[3] = { ll_def, of_def, ml_def };
    static const int ndef[3] = { 36, 29, 53 }, deflog[3] = { 6, 5, 6 }, maxlog[3] = { 9, 8, 9 };
    uint32_t count[53] = { 0 };
    int nsym = 0, distinct = 0, single = -1;
    for (size_t i = 0; i < ns; i++) {
        int k = seq_codes(&sq[i], which);
        if (!count[k]++) {
            distinct++;
            single = k;
        }
        if (k + 1 > nsym) nsym = k + 1;
    }
    if (distinct == 1 && ns > 1) {
        c->rle = 1;
        c->log = 0;
        eb_byte(out, (uint8_t)single);
        return 1;
    }
    int ok_def = 1;
    for (int k = 0; k < nsym; k++)
        if (count[k] && (k >= ndef[which] || defs[which][k] == 0)) ok_def = 0;
    if (ns < 32 && ok_def) {
        fse_cbuild(c, defs[which], ndef[which], deflog[which]);
        return 0;
    }
    int log = hb32((uint32_t)ns) + 1;
    if (log > maxlog[which]) log = maxlog[which];
    if (log < 5) log = 5;
    while ((1 << log) < 2 * distinct && log < maxlog[which]) log++;
    int16_t norm[53];
    fse_normalize(norm, count, nsym, log, (uint32_t)ns);
    fse_write_ncount(out, norm, nsym, log);
    fse_cbuild(c, norm, nsym, log);
    return 2;
}

static void write_sequences(ebuf *out, const seq *sq, size_t ns) {
    if (ns < 128) {
        eb_byte(out, (uint8_t)ns);
    } else if (ns < 0x7F00) {
        eb_byte(out, (uint8_t)((ns >> 8) + 128));
        eb_byte(out, (uint8_t)ns);
    } else {
        eb_byte(out, 255);
        eb_le(out, ns - 0x7F00, 2);
    }
    if (ns == 0) return;
    size_t modes_at = out->n;
    eb_byte(out, 0);
    fse_ctab ll, of, ml;
    int mll = seq_table(out, &ll, sq, ns, 0);
    int mof = seq_table(out, &of, sq, ns, 1);
    int mml = seq_table(out, &ml, sq, ns, 2);
    if (!out->oom) out->p[modes_at] = (uint8_t)(mll << 6 | mof << 4 | mml << 2);
    bitw w = { 0, 0, out };
    size_t n = ns - 1;
    int llc = ll_code(sq[n].ll), mlc = ml_code(sq[n].ml);
    uint32_t ofv = sq[n].ofv;
    int ofc = hb32(ofv);
    uint32_t sml = fse_init(&ml, mlc), sof = fse_init(&of, ofc), sll = fse_init(&ll, llc);
    bw_add(&w, sq[n].ll - llb[llc], llx[llc]);
    bw_add(&w, sq[n].ml - mlb[mlc], mlx[mlc]);
    bw_add(&w, ofv - (1u << ofc), ofc);
    while (n-- > 0) {
        llc = ll_code(sq[n].ll);
        mlc = ml_code(sq[n].ml);
        ofv = sq[n].ofv;
        ofc = hb32(ofv);
        fse_enc(&w, &of, &sof, ofc);
        fse_enc(&w, &ml, &sml, mlc);
        fse_enc(&w, &ll, &sll, llc);
        bw_add(&w, sq[n].ll - llb[llc], llx[llc]);
        bw_add(&w, sq[n].ml - mlb[mlc], mlx[mlc]);
        bw_add(&w, ofv - (1u << ofc), ofc);
    }
    fse_flush(&w, &ml, sml);
    fse_flush(&w, &of, sof);
    fse_flush(&w, &ll, sll);
    bw_close(&w);
}

/* ---- match finder ---- */

#define HLOG 17
#define BLOCK_MAX 131072

typedef struct {
    int32_t head[1 << HLOG];
    int32_t *chain;    /* previous position with the same hash, by position */
    int depth;
    int lazy;
    size_t window;
    uint32_t rep[3];   /* repeat offsets, as the decoder tracks them */
} mfind;

static inline uint32_t rd32le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline uint32_t hash4(const uint8_t *p) { return (rd32le(p) * 2654435761u) >> (32 - HLOG); }

static void mf_insert(mfind *m, const uint8_t *src, size_t i) {
    uint32_t h = hash4(src + i);
    m->chain[i] = m->head[h];
    m->head[h] = (int32_t)i;
}

/* Longest match for position i (bounded by lim); length 0 if none >= 4. */
static size_t mf_best(const mfind *m, const uint8_t *src, size_t i, size_t lim, size_t *off) {
    size_t best = 0;
    int32_t cand = m->head[hash4(src + i)];
    for (int d = 0; d < m->depth && cand >= 0; d++, cand = m->chain[cand]) {
        size_t c = (size_t)cand;
        if (i - c > m->window) break;
        if (rd32le(src + c) != rd32le(src + i)) continue;
        size_t l = 4;
        while (i + l < lim && src[c + l] == src[i + l]) l++;
        if (l > best) {
            best = l;
            *off = i - c;
            if (i + l >= lim) break;
        }
    }
    return best;
}

/* Length of the match at offset off for position i (0 if < 4). */
static size_t match_at(const uint8_t *src, size_t i, size_t off, size_t lim) {
    if (off == 0 || off > i || i + 4 > lim || rd32le(src + i - off) != rd32le(src + i)) return 0;
    size_t l = 4;
    while (i + l < lim && src[i - off + l] == src[i + l]) l++;
    return l;
}

/* The Offset_Value for offset off after ll literals, updating the repeat
 * history like the decoder (RFC 8878 3.1.2.5). */
static uint32_t code_offset(mfind *m, size_t ll, size_t off) {
    uint32_t o = (uint32_t)off;
    if (ll > 0) {
        if (o == m->rep[0]) return 1;
        if (o == m->rep[1]) {
            m->rep[1] = m->rep[0];
            m->rep[0] = o;
            return 2;
        }
        if (o == m->rep[2]) {
            m->rep[2] = m->rep[1];
            m->rep[1] = m->rep[0];
            m->rep[0] = o;
            return 3;
        }
    }
    m->rep[2] = m->rep[1];
    m->rep[1] = m->rep[0];
    m->rep[0] = o;
    return o + 3;
}

/* Compresses one block [start, end) of src into *out (block content);
 * sequences and literals buffers are reused. */
static void compress_block(mfind *m, const uint8_t *src, size_t start, size_t end, ebuf *out, seq *sq, uint8_t *lits) {
    size_t ns = 0, nl = 0, anchor = start, i = start;
    size_t lim = end;
    while (i + 4 <= lim) {
        size_t off = 0, len = 0;
        /* a repeat offset is cheap: take it when it is about as long */
        size_t rl = i > anchor && m->rep[0] <= m->window ? match_at(src, i, m->rep[0], lim) : 0;
        if (rl >= 4) {
            off = m->rep[0];
            len = rl;
            mf_insert(m, src, i);
        } else {
            len = mf_best(m, src, i, lim, &off);
            if (len >= 4 && m->lazy && i + 5 <= lim) {
                mf_insert(m, src, i);
                size_t off2 = 0, len2 = mf_best(m, src, i + 1, lim, &off2);
                if (len2 > len + 1) {
                    i++;
                    len = len2;
                    off = off2;
                }
            } else {
                mf_insert(m, src, i);
            }
        }
        if (len < 4) {
            i++;
            continue;
        }
        size_t ll = i - anchor;
        memcpy(lits + nl, src + anchor, ll);
        nl += ll;
        sq[ns].ll = (uint32_t)ll;
        sq[ns].ml = (uint32_t)len;
        sq[ns].ofv = code_offset(m, ll, off);
        ns++;
        size_t stop = i + len;
        for (size_t k = i + 1; k < stop && k + 4 <= lim; k++) mf_insert(m, src, k);
        i = stop;
        anchor = i;
    }
    for (; i + 4 <= end; i++) mf_insert(m, src, i);
    memcpy(lits + nl, src + anchor, end - anchor);
    nl += end - anchor;
    write_literals(out, lits, nl);
    write_sequences(out, sq, ns);
}

static int is_rle(const uint8_t *p, size_t n) {
    for (size_t i = 1; i < n; i++)
        if (p[i] != p[0]) return 0;
    return n > 0;
}

/* Encodes data as one frame; NULL on out of memory. */
static uint8_t *zstd_encode(const uint8_t *src, size_t n, int level, int checksum, size_t *outlen) {
    ebuf out = { 0 };
    eb_le(&out, 0xFD2FB528u, 4);
    int single = n <= ((size_t)8 << 20);
    int fcsFlag = n < 256 ? 0 : n < 65792 ? 1 : n <= 0xFFFFFFFFu ? 2 : 3;
    if (!single && fcsFlag == 0) fcsFlag = 2;
    eb_byte(&out, (uint8_t)(fcsFlag << 6 | single << 5 | (checksum ? 4 : 0)));
    size_t window = n;
    if (!single) {
        eb_byte(&out, (uint8_t)((23 - 10) << 3));
        window = (size_t)1 << 23;
    }
    if (fcsFlag == 0) eb_byte(&out, (uint8_t)n);
    else if (fcsFlag == 1) eb_le(&out, n - 256, 2);
    else if (fcsFlag == 2) eb_le(&out, n, 4);
    else eb_le(&out, n, 8);
    mfind *m = NULL;
    seq *sq = NULL;
    uint8_t *lits = NULL;
    ebuf blk = { 0 };
    if (n > 0) {
        m = malloc(sizeof *m);
        sq = malloc(sizeof(seq) * (BLOCK_MAX / 4 + 1));
        lits = malloc(BLOCK_MAX + 8);
        if (!m || !sq || !lits || !(m->chain = malloc(sizeof(int32_t) * n))) {
            if (m) m->chain = NULL;
            goto oom;
        }
        memset(m->head, 0xff, sizeof m->head);
        m->depth = level <= 1 ? 1 : level <= 3 ? 4 : level <= 6 ? 16 : level <= 9 ? 64 : 256;
        m->lazy = level >= 5;
        m->window = window;
        m->rep[0] = 1;
        m->rep[1] = 4;
        m->rep[2] = 8;
    }
    size_t pos = 0;
    do {
        size_t bs = n - pos < BLOCK_MAX ? n - pos : BLOCK_MAX;
        int last = pos + bs >= n;
        uint32_t hdr;
        if (bs > 0 && is_rle(src + pos, bs)) {
            hdr = (uint32_t)last | (1u << 1) | (uint32_t)bs << 3;
            eb_le(&out, hdr, 3);
            eb_byte(&out, src[pos]);
            for (size_t i = pos; i + 4 <= pos + bs; i++) mf_insert(m, src, i);
        } else {
            blk.n = 0;
            uint32_t saved[3] = { 0, 0, 0 };
            if (m) memcpy(saved, m->rep, sizeof saved);
            if (bs > 0) compress_block(m, src, pos, pos + bs, &blk, sq, lits);
            if (blk.oom) goto oom;
            if (bs > 0 && blk.n >= bs) memcpy(m->rep, saved, sizeof saved);   /* raw block: no sequences */
            if (bs > 0 && blk.n < bs) {
                hdr = (uint32_t)last | (2u << 1) | (uint32_t)blk.n << 3;
                eb_le(&out, hdr, 3);
                eb_bytes(&out, blk.p, blk.n);
            } else {
                hdr = (uint32_t)last | (uint32_t)bs << 3;
                eb_le(&out, hdr, 3);
                eb_bytes(&out, src + pos, bs);
            }
        }
        pos += bs;
    } while (pos < n);
    if (checksum) eb_le(&out, zell__zstd_xxh64(src, n) & 0xFFFFFFFFu, 4);
    if (out.oom) goto oom;
    if (m) free(m->chain);
    free(m);
    free(sq);
    free(lits);
    free(blk.p);
    *outlen = out.n;
    return out.p;
oom:
    if (m) free(m->chain);
    free(m);
    free(sq);
    free(lits);
    free(blk.p);
    free(out.p);
    return NULL;
}

/* compress.rtZstdEncode(data, level, checksum) []byte! */
void zell_compress_rt_zstd_encode(const zell_slice *data, int64_t level, bool checksum, zell_slice *out,
                                   zell_error **err) {
    memset(out, 0, sizeof *out);
    size_t n = data ? (size_t)data->len : 0, olen = 0;
    const uint8_t *p = n ? (const uint8_t *)data->data : (const uint8_t *)"";
    uint8_t *r = zstd_encode(p, n, (int)level, checksum, &olen);
    if (!r) {
        if (err) *err = zell_error_new_cstr("zstd: out of memory");
        return;
    }
    zell_slice_from(out, 1, NULL, r, (int64_t)olen);
    free(r);
}
