/* Tests for the C halves of the tier-2 stdlib (docs/STDLIB.md level 2):
 * std_regex.c (Unicode tables, folding, search helpers) and std_compress.c
 * (deflate/zlib/gzip/zstd hot loops). Prototypes are declared here: the
 * functions are private to their Zell namespaces (rt* externs). */
#include "test.h"

void zell_regex_rt_unicode(const zell_string *name, zell_slice *out);
int64_t zell_regex_rt_fold(int64_t c);
void zell_regex_rt_ints(int64_t n, zell_slice *out);
void zell_regex_rt_bytes(int64_t n, zell_slice *out);
int64_t zell_regex_rt_index(const zell_string *s, const zell_string *sub, int64_t from);
bool zell_regex_rt_try_lock(int64_t *slot);
void zell_regex_rt_unlock(int64_t *slot);

int64_t zell_compress_rt_crc32(int64_t crc, const zell_slice *data);
int64_t zell_compress_rt_adler32(int64_t adler, const zell_slice *data);
void zell_compress_rt_inflate_new(zell_slice *out);
void zell_compress_rt_inflate(const zell_slice *st, const zell_slice *input, zell_slice *out, zell_error **err);
bool zell_compress_rt_inflate_done(const zell_slice *st);
void zell_compress_rt_inflate_tail(const zell_slice *st, zell_slice *out);
int64_t zell_compress_rt_inflate_unused(const zell_slice *st);
void zell_compress_rt_deflate(const zell_slice *dict, const zell_slice *data, int64_t level, bool final, zell_slice *out);
void zell_compress_rt_zstd_decode(const zell_slice *data, zell_slice *out, zell_error **err);
int64_t zell_compress_rt_zstd_frame_len(const zell_slice *data);
int64_t zell_compress_rt_xxh64(const zell_slice *data);

static int in_class(const zell_slice *r, int64_t c) {
    const int64_t *d = r->data;
    for (int64_t i = 0; i < r->len; i += 2)
        if (c >= d[i] && c <= d[i + 1]) return 1;
    return 0;
}

TEST(regex_unicode_tables) {
    zell_string *n = S("L");
    zell_slice r;
    zell_regex_rt_unicode(n, &r);
    assert(r.len > 100 && r.len % 2 == 0);
    assert(in_class(&r, 'a') && in_class(&r, 0x4E16) && in_class(&r, 0x3B1) && !in_class(&r, '1') && !in_class(&r, ' '));
    const int64_t *d = r.data;
    for (int64_t i = 2; i < r.len; i += 2) assert(d[i] > d[i - 1] + 1); /* sorted, merged */
    zell_slice_release(&r);
    zell_release(n);
    n = S("Greek");
    zell_regex_rt_unicode(n, &r);
    assert(in_class(&r, 0x3B1) && !in_class(&r, 'a'));
    zell_slice_release(&r);
    zell_release(n);
    n = S("Any");
    zell_regex_rt_unicode(n, &r);
    assert(r.len == 2 && ((int64_t *)r.data)[1] == 0x10FFFF);
    zell_slice_release(&r);
    zell_release(n);
    n = S("Klingon");
    zell_regex_rt_unicode(n, &r);
    assert(r.len == 0 && r.backing == NULL);
    zell_release(n);
}

TEST(regex_fold) {
    assert(zell_regex_rt_fold('A') == 'a');
    assert(zell_regex_rt_fold('a') == 'A');
    /* k -> K (Kelvin sign) -> K */
    assert(zell_regex_rt_fold('K') == 'k');
    assert(zell_regex_rt_fold('k') == 0x212A);
    assert(zell_regex_rt_fold(0x212A) == 'K');
    assert(zell_regex_rt_fold(0x3A3) == 0x3C2);
    assert(zell_regex_rt_fold('1') == '1');
}

TEST(regex_helpers) {
    zell_slice s;
    zell_regex_rt_ints(5, &s);
    assert(s.len == 5);
    for (int i = 0; i < 5; i++) assert(((int64_t *)s.data)[i] == 0);
    zell_slice_release(&s);
    zell_regex_rt_ints(0, &s);
    assert(s.len == 0 && s.backing == NULL);
    zell_regex_rt_bytes(3, &s);
    assert(s.len == 3 && ((uint8_t *)s.data)[2] == 0);
    zell_slice_release(&s);
    zell_string *t = S("abcabcx"), *a = S("abc"), *x = S("x"), *e = S("");
    assert(zell_regex_rt_index(t, a, 0) == 0);
    assert(zell_regex_rt_index(t, a, 1) == 3);
    assert(zell_regex_rt_index(t, a, 4) == -1);
    assert(zell_regex_rt_index(t, x, 2) == 6);
    assert(zell_regex_rt_index(t, e, 7) == 7);
    assert(zell_regex_rt_index(t, e, 8) == -1);
    assert(zell_regex_rt_index(NULL, a, 0) == -1);
    zell_release(t); zell_release(a); zell_release(x); zell_release(e);
    int64_t lock = 0;
    assert(zell_regex_rt_try_lock(&lock));
    assert(!zell_regex_rt_try_lock(&lock));
    zell_regex_rt_unlock(&lock);
    assert(zell_regex_rt_try_lock(&lock));
}


static zell_slice bytes_of(const void *p, size_t n) {
    zell_slice s = {0};
    if (n) {
        zell_slice_new(&s, 1, (int64_t)n, (int64_t)n);
        memcpy(s.data, p, n);
    }
    return s;
}

TEST(compress_checksums) {
    zell_slice d = bytes_of("123456789", 9);
    assert(zell_compress_rt_crc32(0, &d) == 0xCBF43926);
    assert(zell_compress_rt_adler32(1, &d) == 0x091E01DE);
    assert(zell_compress_rt_xxh64(&d) == (int64_t)0x8CB841DB40E6AE83ull);
    zell_slice_release(&d);
    zell_slice e = {0};
    assert(zell_compress_rt_crc32(0, &e) == 0);
    assert(zell_compress_rt_xxh64(&e) == (int64_t)0xEF46DB3751D8E999ull);
}

/* Inflates c in chunks of `chunk` bytes. */
static zell_slice inflate_chunked(const zell_slice *c, int64_t chunk, int *done) {
    zell_slice st, out = {0};
    zell_compress_rt_inflate_new(&st);
    for (int64_t i = 0; i < c->len; i += chunk) {
        int64_t k = c->len - i < chunk ? c->len - i : chunk;
        zell_slice in = bytes_of((uint8_t *)c->data + i, (size_t)k), part;
        zell_error *err = NULL;
        zell_compress_rt_inflate(&st, &in, &part, &err);
        assert(err == NULL);
        zell_slice_append_slice(&out, 1, NULL, &part);
        zell_slice_release(&part);
        zell_slice_release(&in);
    }
    *done = zell_compress_rt_inflate_done(&st);
    zell_slice_release(&st);
    return out;
}

TEST(compress_deflate_roundtrip) {
    static uint8_t buf[200000];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)("zell runtime deflate "[i % 22] ^ (i % 1000 == 0 ? 0x20 : 0));
    zell_slice data = bytes_of(buf, sizeof buf), dict = {0};
    for (int level = 0; level <= 9; level += 3) {
        zell_slice c;
        zell_compress_rt_deflate(&dict, &data, level, true, &c);
        assert(c.len > 0 && (level == 0 || c.len < 20000));
        int64_t chunks[] = {1, 100, 1 << 20};
        for (int k = 0; k < 3; k++) {
            if (chunks[k] == 1 && level != 6) continue;
            int done;
            zell_slice out = inflate_chunked(&c, chunks[k], &done);
            assert(done && out.len == data.len && memcmp(out.data, buf, sizeof buf) == 0);
            zell_slice_release(&out);
        }
        zell_slice_release(&c);
    }
    zell_slice_release(&data);
}

TEST(compress_inflate_tail_and_errors) {
    /* fixed block "abc" + 2 trailing bytes */
    zell_slice data = bytes_of("abc", 3), dict = {0}, c;
    zell_compress_rt_deflate(&dict, &data, 6, true, &c);
    uint8_t more[64];
    memcpy(more, c.data, (size_t)c.len);
    more[c.len] = 0xAA;
    more[c.len + 1] = 0xBB;
    zell_slice in = bytes_of(more, (size_t)c.len + 2), st, out, tail;
    zell_compress_rt_inflate_new(&st);
    zell_error *err = NULL;
    zell_compress_rt_inflate(&st, &in, &out, &err);
    assert(err == NULL && out.len == 3 && memcmp(out.data, "abc", 3) == 0);
    assert(zell_compress_rt_inflate_done(&st));
    zell_compress_rt_inflate_tail(&st, &tail);
    int64_t unused = zell_compress_rt_inflate_unused(&st);
    assert(tail.len + unused == 2);
    zell_slice_release(&tail);
    zell_slice_release(&out);
    zell_slice_release(&st);
    zell_slice_release(&in);
    zell_slice_release(&c);
    zell_slice_release(&data);
    uint8_t bad[] = {0x07};
    in = bytes_of(bad, 1);
    zell_compress_rt_inflate_new(&st);
    zell_compress_rt_inflate(&st, &in, &out, &err);
    assert(err != NULL && out.len == 0);
    zell_string *m = zell_error_message(err);
    assert(STR_IS(m, "deflate: invalid block type"));
    zell_release(m);
    zell_release(err);
    zell_slice_release(&st);
    zell_slice_release(&in);
}

TEST(compress_zstd) {
    static const uint8_t frame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x0a, 0x51, 0x00, 0x00, 'f', 'r', 'a', 'm', 'e', ' ', 't', 'w', 'o', '\n'};
    zell_slice in = bytes_of(frame, sizeof frame), out;
    zell_error *err = NULL;
    assert(zell_compress_rt_zstd_frame_len(&in) == (int64_t)sizeof frame);
    zell_compress_rt_zstd_decode(&in, &out, &err);
    assert(err == NULL && out.len == 10 && memcmp(out.data, "frame two\n", 10) == 0);
    zell_slice_release(&out);
    zell_slice_release(&in);
    in = bytes_of(frame, 12);
    assert(zell_compress_rt_zstd_frame_len(&in) == 0);
    zell_compress_rt_zstd_decode(&in, &out, &err);
    assert(err != NULL);
    zell_release(err);
    zell_slice_release(&in);
    in = bytes_of("nope", 4);
    assert(zell_compress_rt_zstd_frame_len(&in) == -1);
    zell_slice_release(&in);
}

int main(void) {
    printf("test_std3\n");
    RUN(regex_unicode_tables);
    RUN(regex_fold);
    RUN(regex_helpers);
    RUN(compress_checksums);
    RUN(compress_deflate_roundtrip);
    RUN(compress_inflate_tail_and_errors);
    RUN(compress_zstd);
    return 0;
}
