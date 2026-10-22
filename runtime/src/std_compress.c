/* std_compress.c - the C half of std.compress (runtime/ABI.md 19.2): the
 * DEFLATE (RFC 1951) decoder and encoder and the CRC-32 / Adler-32
 * checksums used by the zlib (RFC 1950) and gzip (RFC 1952) wrappers. The
 * wrappers, headers and the streaming Reader/Writer types are Zell code
 * (compiler/src/sema/stdlib_src/compress*.zl); zstd is std_zstd.c.
 *
 * Inflate is a resumable state machine whose whole state is one POD
 * struct kept in a Zell []byte (zell_compress_rt_inflate_new), so input
 * can arrive in chunks of any size: every step only consumes bits once
 * everything it needs is buffered. Deflate compresses one chunk per call
 * with the previous 32 KB of input as history; a non-final chunk ends
 * with a sync flush (empty stored block), so the outputs of successive
 * calls concatenate into one stream. */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------- */
/* Checksums                                                              */
/* ---------------------------------------------------------------------- */

static uint32_t crc_tab[8][256];
static zell__plat_once crc_once = ZELL_PLAT_ONCE_INIT;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) crc_tab[t][i] = (crc_tab[t - 1][i] >> 8) ^ crc_tab[0][crc_tab[t - 1][i] & 255];
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    zell__plat_once_run(&crc_once, crc_init);
    uint32_t c = ~crc;
    while (n >= 8) {
        uint32_t a = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        c = crc_tab[7][a & 255] ^ crc_tab[6][(a >> 8) & 255] ^ crc_tab[5][(a >> 16) & 255] ^ crc_tab[4][a >> 24] ^
            crc_tab[3][b & 255] ^ crc_tab[2][(b >> 8) & 255] ^ crc_tab[1][(b >> 16) & 255] ^ crc_tab[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) c = crc_tab[0][(c ^ *p++) & 255] ^ (c >> 8);
    return ~c;
}

/* CRC-32 (IEEE) of data continuing from crc (0 to start). */
int64_t zell_compress_rt_crc32(int64_t crc, const zell_slice *data) {
    if (!data || data->len == 0) return crc & 0xFFFFFFFF;
    return crc32_update((uint32_t)crc, data->data, (size_t)data->len);
}

static uint32_t adler32_update(uint32_t adler, const uint8_t *p, size_t n) {
    uint32_t a = adler & 0xFFFF, b = adler >> 16;
    while (n > 0) {
        size_t k = n < 5552 ? n : 5552;
        n -= k;
        while (k--) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return b << 16 | a;
}

/* Adler-32 of data continuing from adler (1 to start). */
int64_t zell_compress_rt_adler32(int64_t adler, const zell_slice *data) {
    if (!data || data->len == 0) return adler & 0xFFFFFFFF;
    return adler32_update((uint32_t)adler, data->data, (size_t)data->len);
}

/* ---------------------------------------------------------------------- */
/* Output buffer                                                          */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t *p;
    size_t n, cap;
} obuf;

static void ob_grow(obuf *b, size_t need) {
    if (b->n + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->n + need) cap *= 2;
    uint8_t *q = realloc(b->p, cap);
    if (!q) zell_panic("out of memory");
    b->p = q;
    b->cap = cap;
}

static void ob_to_slice(obuf *b, size_t from, zell_slice *out) {
    size_t n = b->n - from;
    if (n == 0) {
        memset(out, 0, sizeof *out);
    } else {
        zell_slice_new(out, 1, (int64_t)n, (int64_t)n);
        memcpy(out->data, b->p + from, n);
    }
    free(b->p);
    b->p = NULL;
}

/* ---------------------------------------------------------------------- */
/* DEFLATE tables                                                         */
/* ---------------------------------------------------------------------- */

static const uint16_t len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
static const uint8_t clen_order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/* ---------------------------------------------------------------------- */
/* Inflate                                                                */
/* ---------------------------------------------------------------------- */

#define FAST_BITS 10

/* A canonical Huffman decoder: a FAST_BITS lookup table (entry =
 * length << 9 | symbol, 0 for longer codes) plus counts/symbols for the
 * slow path. */
typedef struct {
    uint16_t fast[1 << FAST_BITS];
    uint16_t count[16];
    uint16_t sym[320];
    int32_t nsym;
} huff;

enum { M_HEADER, M_STORED_LEN, M_STORED, M_TABLE_SIZES, M_TABLE_CLEN, M_TABLE_LENS, M_CODES, M_DONE };

typedef struct {
    uint32_t magic;
    int32_t mode;
    uint64_t bitbuf;
    int32_t bitcnt;
    int32_t last;
    int32_t stored_left;
    int32_t hlit, hdist, hclen, nlens;
    uint8_t lens[320];
    huff lit, dist, clen;
    uint32_t wfill; /* bytes of history in window (<= 32768) */
    uint32_t tail_n;
    uint8_t tail[16];
    uint8_t window[32768];
} inflate_state;

#define INFLATE_MAGIC 0x494E464Cu

static uint32_t rev_bits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = r << 1 | (v & 1);
        v >>= 1;
    }
    return r;
}

/* Builds h from code lengths; returns 0 if the lengths are over-subscribed
 * (an incomplete code is allowed, as zlib does for single-code distance
 * trees). */
static int huff_build(huff *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return 0;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (lens[i]) h->sym[offs[lens[i]]++] = (uint16_t)i;
    h->nsym = n;
    memset(h->fast, 0, sizeof h->fast);
    /* canonical codes, filled into the fast table bit-reversed */
    int code = 0, k = 0;
    for (int l = 1; l < 16; l++) {
        for (int j = 0; j < h->count[l]; j++, k++, code++) {
            if (l <= FAST_BITS) {
                uint32_t r = rev_bits((uint32_t)code, l);
                for (uint32_t x = r; x < (1u << FAST_BITS); x += 1u << l) h->fast[x] = (uint16_t)(l << 9 | h->sym[k]);
            }
        }
        code <<= 1;
    }
    return 1;
}

/* Decodes a symbol from the low `avail` bits of bits. Returns the symbol
 * and its length in *len, -1 if more bits are needed, -2 if invalid. */
static int huff_decode(const huff *h, uint64_t bits, int avail, int *len) {
    uint16_t e = h->fast[bits & ((1u << FAST_BITS) - 1)];
    if (e) {
        int l = e >> 9;
        if (l > avail) return -1;
        *len = l;
        return e & 511;
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        if (l > avail) return -1;
        code |= (int)((bits >> (l - 1)) & 1);
        int count = h->count[l];
        if (code - count < first) {
            *len = l;
            return h->sym[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static void fixed_tables(inflate_state *s) {
    uint8_t l[320];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    huff_build(&s->lit, l, 288);
    for (i = 0; i < 30; i++) l[i] = 5;
    huff_build(&s->dist, l, 30);
}

void zell_compress_rt_inflate_new(zell_slice *out) {
    zell_slice_new(out, 1, (int64_t)sizeof(inflate_state), (int64_t)sizeof(inflate_state));
    inflate_state *s = out->data;
    s->magic = INFLATE_MAGIC;
    s->mode = M_HEADER;
}

static inflate_state *istate(const zell_slice *st) {
    if (!st || st->len != (int64_t)sizeof(inflate_state)) zell_panic("compress: invalid inflate state");
    inflate_state *s = st->data;
    if (s->magic != INFLATE_MAGIC) zell_panic("compress: invalid inflate state");
    return s;
}

static void ifail(zell_error **err, const char *msg) {
    if (err) *err = zell_error_new_cstr(msg);
}

/* Decompresses the input chunk; appends the output. */
void zell_compress_rt_inflate(const zell_slice *st, const zell_slice *input, zell_slice *out, zell_error **err) {
    inflate_state *s = istate(st);
    if (err) *err = NULL;
    const uint8_t *in = input && input->len ? input->data : NULL;
    size_t in_n = input ? (size_t)input->len : 0, ip = 0;
    if (s->mode == M_DONE) {
        /* data after the end of the stream: keep it as tail (bounded) */
        memset(out, 0, sizeof *out);
        return;
    }
    obuf ob = {0};
    ob_grow(&ob, s->wfill + in_n * 4 + 1024);
    memcpy(ob.p, s->window, s->wfill);
    ob.n = s->wfill;
    size_t start = s->wfill;
    uint64_t bb = s->bitbuf;
    int bc = s->bitcnt;
#define REFILL() while (bc <= 56 && ip < in_n) { bb |= (uint64_t)in[ip++] << bc; bc += 8; }
#define DROP(n) do { bb >>= (n); bc -= (n); } while (0)
    for (;;) {
        REFILL();
        if (s->mode == M_HEADER) {
            if (bc < 3) break;
            s->last = (int)(bb & 1);
            int type = (int)((bb >> 1) & 3);
            DROP(3);
            if (type == 0) {
                DROP(bc & 7);
                s->mode = M_STORED_LEN;
            } else if (type == 1) {
                fixed_tables(s);
                s->mode = M_CODES;
            } else if (type == 2) {
                s->mode = M_TABLE_SIZES;
            } else {
                ifail(err, "deflate: invalid block type");
                goto fail;
            }
        } else if (s->mode == M_STORED_LEN) {
            if (bc < 32) break;
            uint32_t len = (uint32_t)(bb & 0xFFFF), nlen = (uint32_t)((bb >> 16) & 0xFFFF);
            if ((len ^ 0xFFFF) != nlen) {
                ifail(err, "deflate: invalid stored block length");
                goto fail;
            }
            DROP(32);
            s->stored_left = (int32_t)len;
            s->mode = M_STORED;
        } else if (s->mode == M_STORED) {
            /* bytes still in the bit buffer first, then straight from input */
            while (s->stored_left > 0 && bc >= 8) {
                ob_grow(&ob, 1);
                ob.p[ob.n++] = (uint8_t)bb;
                DROP(8);
                s->stored_left--;
            }
            if (s->stored_left > 0) {
                size_t k = in_n - ip;
                if (k > (size_t)s->stored_left) k = (size_t)s->stored_left;
                ob_grow(&ob, k);
                memcpy(ob.p + ob.n, in + ip, k);
                ob.n += k;
                ip += k;
                s->stored_left -= (int32_t)k;
                if (s->stored_left > 0) break;
            }
            s->mode = s->last ? M_DONE : M_HEADER;
            if (s->mode == M_DONE) break;
        } else if (s->mode == M_TABLE_SIZES) {
            if (bc < 14) break;
            s->hlit = (int32_t)(bb & 31) + 257;
            s->hdist = (int32_t)((bb >> 5) & 31) + 1;
            s->hclen = (int32_t)((bb >> 10) & 15) + 4;
            DROP(14);
            if (s->hlit > 286 || s->hdist > 30) {
                ifail(err, "deflate: too many length or distance codes");
                goto fail;
            }
            s->mode = M_TABLE_CLEN;
        } else if (s->mode == M_TABLE_CLEN) {
            if (bc < s->hclen * 3) {
                if (ip < in_n) continue;
                break;
            }
            uint8_t cl[19] = {0};
            for (int i = 0; i < s->hclen; i++) {
                cl[clen_order[i]] = (uint8_t)(bb & 7);
                DROP(3);
            }
            if (!huff_build(&s->clen, cl, 19)) {
                ifail(err, "deflate: invalid code-length code");
                goto fail;
            }
            s->nlens = 0;
            s->mode = M_TABLE_LENS;
        } else if (s->mode == M_TABLE_LENS) {
            int total = s->hlit + s->hdist;
            int stalled = 0;
            while (s->nlens < total) {
                REFILL();
                int l;
                int sym = huff_decode(&s->clen, bb, bc, &l);
                if (sym == -1) { stalled = 1; break; }
                if (sym < 0) {
                    ifail(err, "deflate: invalid code-length symbol");
                    goto fail;
                }
                if (sym < 16) {
                    DROP(l);
                    s->lens[s->nlens++] = (uint8_t)sym;
                    continue;
                }
                int eb = sym == 16 ? 2 : sym == 17 ? 3 : 7;
                if (l + eb > bc) { stalled = 1; break; }
                int rep = (int)((bb >> l) & ((1u << eb) - 1));
                DROP(l + eb);
                int count, val = 0;
                if (sym == 16) {
                    if (s->nlens == 0) {
                        ifail(err, "deflate: repeat of no code length");
                        goto fail;
                    }
                    val = s->lens[s->nlens - 1];
                    count = 3 + rep;
                } else if (sym == 17) {
                    count = 3 + rep;
                } else {
                    count = 11 + rep;
                }
                if (s->nlens + count > total) {
                    ifail(err, "deflate: too many code lengths");
                    goto fail;
                }
                while (count--) s->lens[s->nlens++] = (uint8_t)val;
            }
            if (stalled) break;
            if (s->lens[256] == 0) {
                ifail(err, "deflate: missing end-of-block code");
                goto fail;
            }
            if (!huff_build(&s->lit, s->lens, s->hlit) || !huff_build(&s->dist, s->lens + s->hlit, s->hdist)) {
                ifail(err, "deflate: invalid literal/length or distance code");
                goto fail;
            }
            s->mode = M_CODES;
        } else if (s->mode == M_CODES) {
            int stalled = 0;
            for (;;) {
                REFILL();
                int l1;
                int sym = huff_decode(&s->lit, bb, bc, &l1);
                if (sym == -1) { stalled = 1; break; }
                if (sym < 0) {
                    ifail(err, "deflate: invalid literal/length code");
                    goto fail;
                }
                if (sym < 256) {
                    DROP(l1);
                    ob_grow(&ob, 1);
                    ob.p[ob.n++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 256) {
                    DROP(l1);
                    s->mode = s->last ? M_DONE : M_HEADER;
                    break;
                }
                sym -= 257;
                if (sym >= 29) {
                    ifail(err, "deflate: invalid length code");
                    goto fail;
                }
                int e1 = len_extra[sym];
                if (l1 + e1 > bc) { stalled = 1; break; }
                int len = len_base[sym] + (int)((bb >> l1) & ((1u << e1) - 1));
                int l2;
                int ds = huff_decode(&s->dist, bb >> (l1 + e1), bc - l1 - e1, &l2);
                if (ds == -1) { stalled = 1; break; }
                if (ds < 0 || ds >= 30) {
                    ifail(err, "deflate: invalid distance code");
                    goto fail;
                }
                int e2 = dist_extra[ds];
                if (l1 + e1 + l2 + e2 > bc) { stalled = 1; break; }
                int dist = dist_base[ds] + (int)((bb >> (l1 + e1 + l2)) & ((1u << e2) - 1));
                DROP(l1 + e1 + l2 + e2);
                if ((size_t)dist > ob.n) {
                    ifail(err, "deflate: distance too far back");
                    goto fail;
                }
                ob_grow(&ob, (size_t)len);
                uint8_t *d = ob.p + ob.n;
                const uint8_t *src = d - dist;
                if (dist >= len) {
                    memcpy(d, src, (size_t)len);
                } else {
                    for (int i = 0; i < len; i++) d[i] = src[i];
                }
                ob.n += (size_t)len;
            }
            if (stalled) break;
            if (s->mode == M_DONE) break;
        } else {
            break;
        }
    }
#undef REFILL
#undef DROP
    if (s->mode == M_DONE) {
        /* whole bytes left in the bit buffer go back to the tail */
        int extra = bc & 7;
        bb >>= extra;
        bc -= extra;
        s->tail_n = 0;
        while (bc >= 8 && s->tail_n < sizeof s->tail) {
            s->tail[s->tail_n++] = (uint8_t)bb;
            bb >>= 8;
            bc -= 8;
        }
        bb = 0;
        bc = 0;
        /* input bytes of this call after the end (rt_inflate_unused) */
        s->stored_left = (int32_t)(in_n - ip);
    }
    s->bitbuf = bb;
    s->bitcnt = bc;
    /* the last 32 KB become the history for the next call */
    size_t keep = ob.n < 32768 ? ob.n : 32768;
    memcpy(s->window, ob.p + ob.n - keep, keep);
    s->wfill = (uint32_t)keep;
    ob_to_slice(&ob, start, out);
    return;
fail:
    free(ob.p);
    memset(out, 0, sizeof *out);
    s->mode = M_DONE;
}

bool zell_compress_rt_inflate_done(const zell_slice *st) {
    return istate(st)->mode == M_DONE;
}

/* After the end of the stream: the whole bytes that were read ahead into
 * the bit buffer (they precede the unused rest of the last input chunk). */
void zell_compress_rt_inflate_tail(const zell_slice *st, zell_slice *out) {
    inflate_state *s = istate(st);
    if (s->mode != M_DONE || s->tail_n == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 1, s->tail_n, s->tail_n);
    memcpy(out->data, s->tail, s->tail_n);
}

/* After the end of the stream: how many bytes at the end of the last
 * input chunk were not consumed. */
int64_t zell_compress_rt_inflate_unused(const zell_slice *st) {
    inflate_state *s = istate(st);
    return s->mode == M_DONE ? s->stored_left : 0;
}

/* ---------------------------------------------------------------------- */
/* Deflate                                                                */
/* ---------------------------------------------------------------------- */

typedef struct {
    obuf ob;
    uint64_t bb;
    int bc;
} bitw;

static void bw_put(bitw *w, uint32_t bits, int n) {
    w->bb |= (uint64_t)bits << w->bc;
    w->bc += n;
    if (w->bc >= 32) {
        ob_grow(&w->ob, 4);
        uint8_t *p = w->ob.p + w->ob.n;
        p[0] = (uint8_t)w->bb;
        p[1] = (uint8_t)(w->bb >> 8);
        p[2] = (uint8_t)(w->bb >> 16);
        p[3] = (uint8_t)(w->bb >> 24);
        w->ob.n += 4;
        w->bb >>= 32;
        w->bc -= 32;
    }
}

static void bw_align(bitw *w) {
    while (w->bc > 0) {
        ob_grow(&w->ob, 1);
        w->ob.p[w->ob.n++] = (uint8_t)w->bb;
        w->bb >>= 8;
        w->bc = w->bc > 8 ? w->bc - 8 : 0;
    }
    w->bb = 0;
    w->bc = 0;
}

/* Moffat & Katajainen in-place minimum-redundancy code lengths: a[] holds
 * ascending frequencies, receives the code lengths. */
static void min_redundancy(int *a, int n) {
    if (n == 0) return;
    if (n == 1) {
        a[0] = 1;
        return;
    }
    a[0] += a[1];
    int root = 0, leaf = 2, next;
    for (next = 1; next < n - 1; next++) {
        if (leaf >= n || a[root] < a[leaf]) {
            a[next] = a[root];
            a[root++] = next;
        } else {
            a[next] = a[leaf++];
        }
        if (leaf >= n || (root < next && a[root] < a[leaf])) {
            a[next] += a[root];
            a[root++] = next;
        } else {
            a[next] += a[leaf++];
        }
    }
    a[n - 2] = 0;
    for (next = n - 3; next >= 0; next--) a[next] = a[a[next]] + 1;
    int avbl = 1, used = 0, dpth = 0;
    root = n - 2;
    next = n - 1;
    while (avbl > 0) {
        while (root >= 0 && a[root] == dpth) {
            used++;
            root--;
        }
        while (avbl > used) {
            a[next--] = dpth;
            avbl--;
        }
        avbl = 2 * used;
        dpth++;
        used = 0;
    }
}

typedef struct {
    uint32_t key;
    uint16_t sym;
} symfreq;

static int symfreq_cmp(const void *x, const void *y) {
    const symfreq *a = x, *b = y;
    if (a->key != b->key) return a->key < b->key ? -1 : 1;
    return a->sym < b->sym ? -1 : a->sym > b->sym;
}

/* Length-limited Huffman code lengths for freq[0..n). */
static void build_lengths(const uint32_t *freq, int n, int limit, uint8_t *lens) {
    symfreq syms[320];
    int used = 0;
    memset(lens, 0, (size_t)n);
    for (int i = 0; i < n; i++)
        if (freq[i]) {
            syms[used].key = freq[i];
            syms[used].sym = (uint16_t)i;
            used++;
        }
    if (used == 0) return;
    if (used == 1) {
        /* a one-symbol code is incomplete: pair it with a dummy symbol */
        lens[syms[0].sym] = 1;
        lens[syms[0].sym == 0 ? 1 : 0] = 1;
        return;
    }
    qsort(syms, (size_t)used, sizeof syms[0], symfreq_cmp);
    int a[320] = {0};
    for (int i = 0; i < used; i++) a[i] = (int)syms[i].key;
    min_redundancy(a, used);
    int num[33] = {0};
    for (int i = 0; i < used; i++) num[a[i] < 32 ? a[i] : 32]++;
    for (int i = limit + 1; i <= 32; i++) num[limit] += num[i];
    uint32_t total = 0;
    for (int i = limit; i > 0; i--) total += (uint32_t)num[i] << (limit - i);
    while (total != (1u << limit)) {
        num[limit]--;
        for (int i = limit - 1; i > 0; i--)
            if (num[i]) {
                num[i]--;
                num[i + 1] += 2;
                break;
            }
        total--;
    }
    int j = used;
    for (int l = 1; l <= limit; l++)
        for (int k = num[l]; k > 0; k--) lens[syms[--j].sym] = (uint8_t)l;
}

/* Canonical codes (bit-reversed for LSB-first output). */
static void build_codes(const uint8_t *lens, int n, uint16_t *codes) {
    int count[16] = {0}, next[16];
    for (int i = 0; i < n; i++) count[lens[i]]++;
    count[0] = 0;
    int code = 0;
    for (int l = 1; l < 16; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    for (int i = 0; i < n; i++)
        if (lens[i]) codes[i] = (uint16_t)rev_bits((uint32_t)next[lens[i]]++, lens[i]);
}

static int len_code(int len) {
    int c = 0;
    while (c < 28 && len_base[c + 1] <= len) c++;
    return c;
}

static int dist_code(int d) {
    if (d <= 4) return d - 1;
    int c = 4;
    while (c < 29 && dist_base[c + 1] <= d) c++;
    return c;
}

/* Symbols of one block: literals (dist == 0) and matches. */
typedef struct {
    uint16_t len;  /* literal byte or match length */
    uint16_t dist; /* 0 for a literal */
} lzsym;

typedef struct {
    bitw w;
    lzsym *syms;
    int nsyms;
    const uint8_t *buf;
    size_t block_start; /* buffer offset where the current block's bytes start */
    uint8_t lcode[300];
} denc;

#define BLOCK_SYMS 16384

static void write_block(denc *e, size_t block_end, int last) {
    uint32_t lf[286] = {0}, df[30] = {0};
    for (int i = 0; i < e->nsyms; i++) {
        lzsym s = e->syms[i];
        if (s.dist == 0) {
            lf[s.len]++;
        } else {
            lf[257 + e->lcode[s.len]]++;
            df[dist_code(s.dist)]++;
        }
    }
    lf[256] = 1;
    uint8_t ll[288] = {0}, dl[30];
    build_lengths(lf, 286, 15, ll);
    build_lengths(df, 30, 15, dl);
    int hlit = 286;
    while (hlit > 257 && ll[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && dl[hdist - 1] == 0) hdist--;
    /* run-length encoding of the code lengths */
    uint8_t all[316];
    memcpy(all, ll, (size_t)hlit);
    memcpy(all + hlit, dl, (size_t)hdist);
    int total = hlit + hdist;
    uint8_t rle[316];
    uint8_t rle_extra[316];
    int nr = 0;
    for (int i = 0; i < total;) {
        int v = all[i], run = 1;
        while (i + run < total && all[i + run] == v) run++;
        if (v == 0 && run >= 3) {
            int r = run > 138 ? 138 : run;
            if (r >= 11) {
                rle[nr] = 18;
                rle_extra[nr++] = (uint8_t)(r - 11);
            } else {
                rle[nr] = 17;
                rle_extra[nr++] = (uint8_t)(r - 3);
            }
            i += r;
        } else if (v != 0 && run >= 4) {
            rle[nr] = (uint8_t)v;
            rle_extra[nr++] = 0;
            int r = run - 1 > 6 ? 6 : run - 1;
            rle[nr] = 16;
            rle_extra[nr++] = (uint8_t)(r - 3);
            i += 1 + r;
        } else {
            rle[nr] = (uint8_t)v;
            rle_extra[nr++] = 0;
            i++;
        }
    }
    uint32_t cf[19] = {0};
    for (int i = 0; i < nr; i++) cf[rle[i]]++;
    uint8_t cl[19];
    build_lengths(cf, 19, 7, cl);
    int hclen = 19;
    while (hclen > 4 && cl[clen_order[hclen - 1]] == 0) hclen--;
    /* sizes in bits */
    uint64_t dyn = 3 + 14 + (uint64_t)hclen * 3, fix = 3, raw_bytes = block_end - e->block_start;
    for (int i = 0; i < nr; i++) dyn += cl[rle[i]] + (rle[i] == 16 ? 2 : rle[i] == 17 ? 3 : rle[i] == 18 ? 7 : 0);
    for (int i = 0; i < 286; i++) {
        uint64_t f = lf[i];
        if (!f) continue;
        int eb = i >= 257 ? len_extra[i - 257] : 0;
        dyn += f * (ll[i] + eb);
        fix += f * ((i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8) + eb);
    }
    for (int i = 0; i < 30; i++) {
        dyn += (uint64_t)df[i] * (dl[i] + dist_extra[i]);
        fix += (uint64_t)df[i] * (5 + dist_extra[i]);
    }
    uint64_t stored = (raw_bytes + 5 * (raw_bytes / 65535 + 1)) * 8 + 7;
    bitw *w = &e->w;
    if (stored <= dyn && stored <= fix) {
        size_t off = e->block_start;
        do {
            size_t k = block_end - off > 65535 ? 65535 : block_end - off;
            int fin = last && off + k == block_end;
            bw_put(w, (uint32_t)fin, 3);
            bw_align(w);
            ob_grow(&w->ob, 4 + k);
            uint8_t *p = w->ob.p + w->ob.n;
            p[0] = (uint8_t)k;
            p[1] = (uint8_t)(k >> 8);
            p[2] = (uint8_t)~k;
            p[3] = (uint8_t)(~k >> 8);
            memcpy(p + 4, e->buf + off, k);
            w->ob.n += 4 + k;
            off += k;
        } while (off < block_end);
        e->nsyms = 0;
        e->block_start = block_end;
        return;
    }
    uint16_t lc[288], dc[30];
    int nlit = 286;
    if (fix < dyn) {
        nlit = 288;
        for (int i = 0; i < 288; i++) ll[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        for (int i = 0; i < 30; i++) dl[i] = 5;
        bw_put(w, (uint32_t)last | 2, 3);
    } else {
        uint16_t cc[19];
        build_codes(cl, 19, cc);
        bw_put(w, (uint32_t)last | 4, 3);
        bw_put(w, (uint32_t)(hlit - 257), 5);
        bw_put(w, (uint32_t)(hdist - 1), 5);
        bw_put(w, (uint32_t)(hclen - 4), 4);
        for (int i = 0; i < hclen; i++) bw_put(w, cl[clen_order[i]], 3);
        for (int i = 0; i < nr; i++) {
            bw_put(w, cc[rle[i]], cl[rle[i]]);
            if (rle[i] == 16) bw_put(w, rle_extra[i], 2);
            else if (rle[i] == 17) bw_put(w, rle_extra[i], 3);
            else if (rle[i] == 18) bw_put(w, rle_extra[i], 7);
        }
    }
    build_codes(ll, nlit, lc);
    build_codes(dl, 30, dc);
    for (int i = 0; i < e->nsyms; i++) {
        lzsym s = e->syms[i];
        if (s.dist == 0) {
            bw_put(w, lc[s.len], ll[s.len]);
        } else {
            int c = e->lcode[s.len];
            bw_put(w, lc[257 + c], ll[257 + c]);
            if (len_extra[c]) bw_put(w, (uint32_t)(s.len - len_base[c]), len_extra[c]);
            int d = dist_code(s.dist);
            bw_put(w, dc[d], dl[d]);
            if (dist_extra[d]) bw_put(w, (uint32_t)(s.dist - dist_base[d]), dist_extra[d]);
        }
    }
    bw_put(w, lc[256], ll[256]);
    e->nsyms = 0;
    e->block_start = block_end;
}

static void emit(denc *e, size_t pos_after, int len, int dist) {
    e->syms[e->nsyms].len = (uint16_t)len;
    e->syms[e->nsyms].dist = (uint16_t)dist;
    e->nsyms++;
    if (e->nsyms >= BLOCK_SYMS) write_block(e, pos_after, 0);
}

/* Level parameters as in zlib: good_length, max_lazy, nice_length, max_chain. */
static const struct {
    int good, lazy, nice, chain;
} level_cfg[10] = {
    {0, 0, 0, 0},       {4, 4, 8, 4},        {4, 5, 16, 8},        {4, 6, 32, 32},       {4, 4, 16, 16},
    {8, 16, 32, 32},    {8, 16, 128, 128},   {8, 32, 128, 256},    {32, 128, 258, 1024}, {32, 258, 258, 4096},
};

#define HBITS 15
#define WSIZE 32768
#define WMASK (WSIZE - 1)

static inline uint32_t hash3(const uint8_t *p) {
    uint32_t v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    return (v * 2654435761u) >> (32 - HBITS);
}

/* Compresses buf[from..n) (buf[0..from) is history) into e. */
static void lz_compress(denc *e, const uint8_t *buf, size_t from, size_t n, int level, int last) {
    int32_t *head = malloc(sizeof(int32_t) << HBITS);
    int32_t *prev = malloc(sizeof(int32_t) * WSIZE);
    if (!head || !prev) zell_panic("out of memory");
    memset(head, 0xFF, sizeof(int32_t) << HBITS);
    int good = level_cfg[level].good, lazy = level_cfg[level].lazy, nice = level_cfg[level].nice, chain0 = level_cfg[level].chain;
    int use_lazy = level >= 4;
#define INSERT(p) do { uint32_t h_ = hash3(buf + (p)); prev[(p) & WMASK] = head[h_]; head[h_] = (int32_t)(p); } while (0)
    for (size_t p = from > WSIZE ? from - WSIZE : 0; p + 2 < from; p++) INSERT(p);
    size_t p = from;
    int prev_len = 0, prev_dist = 0, have_prev = 0;
    while (p < n) {
        int best = 0, bdist = 0;
        if (p + 2 < n) {
            int chain = chain0;
            if (use_lazy && have_prev && prev_len >= good) chain >>= 2;
            size_t maxlen = n - p < 258 ? n - p : 258;
            int32_t c = head[hash3(buf + p)];
            while (c >= 0 && p - (size_t)c <= WSIZE && chain-- > 0) {
                const uint8_t *a = buf + c, *b = buf + p;
                if (a[best] == b[best] && a[0] == b[0] && a[1] == b[1]) {
                    size_t l = 2;
                    while (l < maxlen && a[l] == b[l]) l++;
                    if ((int)l > best) {
                        best = (int)l;
                        bdist = (int)(p - (size_t)c);
                        if (best >= nice || l == maxlen) break;
                    }
                }
                int32_t nc = prev[c & WMASK];
                if (nc >= c) break;
                c = nc;
            }
            if (best < 3) best = 0;
            /* a 3-byte match far away costs more than literals */
            if (best == 3 && bdist > 4096) best = 0;
            INSERT(p);
        }
        if (!use_lazy) {
            if (best >= 3) {
                emit(e, p + (size_t)best, best, bdist);
                size_t end = p + (size_t)best;
                if (best <= lazy) {
                    for (size_t q = p + 1; q < end && q + 2 < n; q++) INSERT(q);
                }
                p = end;
            } else {
                emit(e, p + 1, buf[p], 0);
                p++;
            }
            continue;
        }
        /* lazy matching: keep the previous match if this one is not longer */
        if (have_prev) {
            if (prev_len >= 3 && best <= prev_len) {
                size_t start = p - 1;
                emit(e, start + (size_t)prev_len, prev_len, prev_dist);
                size_t end = start + (size_t)prev_len;
                for (size_t q = p + 1; q < end && q + 2 < n; q++) INSERT(q);
                p = end;
                have_prev = 0;
                prev_len = 0;
                continue;
            }
            emit(e, p, buf[p - 1], 0);
        }
        if (best >= 3 && best >= lazy) {
            /* long enough: take it now */
            emit(e, p + (size_t)best, best, bdist);
            size_t end = p + (size_t)best;
            for (size_t q = p + 1; q < end && q + 2 < n; q++) INSERT(q);
            p = end;
            have_prev = 0;
            prev_len = 0;
            continue;
        }
        have_prev = 1;
        prev_len = best;
        prev_dist = bdist;
        p++;
    }
    if (have_prev) emit(e, n, buf[n - 1], 0);
#undef INSERT
    free(head);
    free(prev);
    (void)last;
}

/* Compresses data (raw DEFLATE) with dict (up to its last 32 KB) as
 * history. final: ends the stream; otherwise ends with a sync flush so the
 * output is byte-aligned and can be followed by the next call's output. */
void zell_compress_rt_deflate(const zell_slice *dict, const zell_slice *data, int64_t level, bool final, zell_slice *out) {
    if (level < 0 || level > 9) level = 6;
    size_t dn = dict ? (size_t)dict->len : 0, n = data ? (size_t)data->len : 0;
    if (dn > WSIZE) dn = WSIZE;
    uint8_t *buf = malloc(dn + n + 1);
    if (!buf) zell_panic("out of memory");
    if (dn) memcpy(buf, (const uint8_t *)dict->data + dict->len - (int64_t)dn, dn);
    if (n) memcpy(buf + dn, data->data, n);
    denc e;
    memset(&e, 0, sizeof e);
    e.buf = buf;
    e.block_start = dn;
    for (int l = 3; l <= 258; l++) e.lcode[l] = (uint8_t)len_code(l);
    if (level == 0 || n == 0) {
        /* stored blocks (an empty final block is one empty stored block) */
        size_t off = dn;
        do {
            size_t k = dn + n - off > 65535 ? 65535 : dn + n - off;
            int fin = final && off + k == dn + n;
            if (k == 0 && !final) break;
            bw_put(&e.w, (uint32_t)fin, 3);
            bw_align(&e.w);
            ob_grow(&e.w.ob, 4 + k);
            uint8_t *p = e.w.ob.p + e.w.ob.n;
            p[0] = (uint8_t)k;
            p[1] = (uint8_t)(k >> 8);
            p[2] = (uint8_t)~k;
            p[3] = (uint8_t)(~k >> 8);
            if (k) memcpy(p + 4, buf + off, k);
            e.w.ob.n += 4 + k;
            off += k;
        } while (off < dn + n);
    } else {
        e.syms = malloc(sizeof(lzsym) * BLOCK_SYMS);
        if (!e.syms) zell_panic("out of memory");
        lz_compress(&e, buf, dn, dn + n, (int)level, final);
        write_block(&e, dn + n, final);
        free(e.syms);
    }
    if (!final) {
        /* sync flush: an empty non-final stored block */
        bw_put(&e.w, 0, 3);
        bw_align(&e.w);
        ob_grow(&e.w.ob, 4);
        uint8_t *p = e.w.ob.p + e.w.ob.n;
        p[0] = 0;
        p[1] = 0;
        p[2] = 0xFF;
        p[3] = 0xFF;
        e.w.ob.n += 4;
    } else {
        bw_align(&e.w);
    }
    free(buf);
    ob_to_slice(&e.w.ob, 0, out);
}

/* Concatenation of two byte slices (the Zell side's buffer helper). */
void zell_compress_rt_concat(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    int64_t na = a ? a->len : 0, nb = b ? b->len : 0;
    if (na + nb == 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 1, na + nb, na + nb);
    if (na) memcpy(out->data, a->data, (size_t)na);
    if (nb) memcpy((uint8_t *)out->data + na, b->data, (size_t)nb);
}

/* b[lo:hi] as a fresh copy (so a large buffer is not kept alive). */
void zell_compress_rt_copy(const zell_slice *b, int64_t lo, int64_t hi, zell_slice *out) {
    int64_t n = b ? b->len : 0;
    if (lo < 0) lo = 0;
    if (hi > n) hi = n;
    if (hi <= lo) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 1, hi - lo, hi - lo);
    memcpy(out->data, (const uint8_t *)b->data + lo, (size_t)(hi - lo));
}

/* A zeroed []byte of length n. */
void zell_compress_rt_zeros(int64_t n, zell_slice *out) {
    if (n <= 0) {
        memset(out, 0, sizeof *out);
        return;
    }
    zell_slice_new(out, 1, n, n);
}
