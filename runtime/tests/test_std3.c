/* Tests for the C halves of the tier-2 stdlib (docs/STDLIB.md level 2):
 * std_regex.c (Unicode tables, folding, search helpers) and std_compress.c
 * (deflate/zlib/gzip/zstd hot loops). Prototypes are declared here: the
 * functions are private to their Slang namespaces (rt* externs). */
#include "test.h"

void slang_regex_rt_unicode(const slang_string *name, slang_slice *out);
int64_t slang_regex_rt_fold(int64_t c);
void slang_regex_rt_ints(int64_t n, slang_slice *out);
void slang_regex_rt_bytes(int64_t n, slang_slice *out);
int64_t slang_regex_rt_index(const slang_string *s, const slang_string *sub, int64_t from);
bool slang_regex_rt_try_lock(int64_t *slot);
void slang_regex_rt_unlock(int64_t *slot);

int64_t slang_compress_rt_crc32(int64_t crc, const slang_slice *data);
int64_t slang_compress_rt_adler32(int64_t adler, const slang_slice *data);
void slang_compress_rt_inflate_new(slang_slice *out);
void slang_compress_rt_inflate(const slang_slice *st, const slang_slice *input, slang_slice *out, slang_error **err);
bool slang_compress_rt_inflate_done(const slang_slice *st);
void slang_compress_rt_inflate_tail(const slang_slice *st, slang_slice *out);
int64_t slang_compress_rt_inflate_unused(const slang_slice *st);
void slang_compress_rt_deflate(const slang_slice *dict, const slang_slice *data, int64_t level, bool final, slang_slice *out);
void slang_compress_rt_zstd_decode(const slang_slice *data, slang_slice *out, slang_error **err);
int64_t slang_compress_rt_zstd_frame_len(const slang_slice *data);
int64_t slang_compress_rt_xxh64(const slang_slice *data);

static int in_class(const slang_slice *r, int64_t c) {
    const int64_t *d = r->data;
    for (int64_t i = 0; i < r->len; i += 2)
        if (c >= d[i] && c <= d[i + 1]) return 1;
    return 0;
}

TEST(regex_unicode_tables) {
    slang_string *n = S("L");
    slang_slice r;
    slang_regex_rt_unicode(n, &r);
    assert(r.len > 100 && r.len % 2 == 0);
    assert(in_class(&r, 'a') && in_class(&r, 0x4E16) && in_class(&r, 0x3B1) && !in_class(&r, '1') && !in_class(&r, ' '));
    const int64_t *d = r.data;
    for (int64_t i = 2; i < r.len; i += 2) assert(d[i] > d[i - 1] + 1); /* sorted, merged */
    slang_slice_release(&r);
    slang_release(n);
    n = S("Greek");
    slang_regex_rt_unicode(n, &r);
    assert(in_class(&r, 0x3B1) && !in_class(&r, 'a'));
    slang_slice_release(&r);
    slang_release(n);
    n = S("Any");
    slang_regex_rt_unicode(n, &r);
    assert(r.len == 2 && ((int64_t *)r.data)[1] == 0x10FFFF);
    slang_slice_release(&r);
    slang_release(n);
    n = S("Klingon");
    slang_regex_rt_unicode(n, &r);
    assert(r.len == 0 && r.backing == NULL);
    slang_release(n);
}

TEST(regex_fold) {
    assert(slang_regex_rt_fold('A') == 'a');
    assert(slang_regex_rt_fold('a') == 'A');
    /* k -> K (Kelvin sign) -> K */
    assert(slang_regex_rt_fold('K') == 'k');
    assert(slang_regex_rt_fold('k') == 0x212A);
    assert(slang_regex_rt_fold(0x212A) == 'K');
    assert(slang_regex_rt_fold(0x3A3) == 0x3C2);
    assert(slang_regex_rt_fold('1') == '1');
}

TEST(regex_helpers) {
    slang_slice s;
    slang_regex_rt_ints(5, &s);
    assert(s.len == 5);
    for (int i = 0; i < 5; i++) assert(((int64_t *)s.data)[i] == 0);
    slang_slice_release(&s);
    slang_regex_rt_ints(0, &s);
    assert(s.len == 0 && s.backing == NULL);
    slang_regex_rt_bytes(3, &s);
    assert(s.len == 3 && ((uint8_t *)s.data)[2] == 0);
    slang_slice_release(&s);
    slang_string *t = S("abcabcx"), *a = S("abc"), *x = S("x"), *e = S("");
    assert(slang_regex_rt_index(t, a, 0) == 0);
    assert(slang_regex_rt_index(t, a, 1) == 3);
    assert(slang_regex_rt_index(t, a, 4) == -1);
    assert(slang_regex_rt_index(t, x, 2) == 6);
    assert(slang_regex_rt_index(t, e, 7) == 7);
    assert(slang_regex_rt_index(t, e, 8) == -1);
    assert(slang_regex_rt_index(NULL, a, 0) == -1);
    slang_release(t); slang_release(a); slang_release(x); slang_release(e);
    int64_t lock = 0;
    assert(slang_regex_rt_try_lock(&lock));
    assert(!slang_regex_rt_try_lock(&lock));
    slang_regex_rt_unlock(&lock);
    assert(slang_regex_rt_try_lock(&lock));
}


static slang_slice bytes_of(const void *p, size_t n) {
    slang_slice s = {0};
    if (n) {
        slang_slice_new(&s, 1, (int64_t)n, (int64_t)n);
        memcpy(s.data, p, n);
    }
    return s;
}

TEST(compress_checksums) {
    slang_slice d = bytes_of("123456789", 9);
    assert(slang_compress_rt_crc32(0, &d) == 0xCBF43926);
    assert(slang_compress_rt_adler32(1, &d) == 0x091E01DE);
    assert(slang_compress_rt_xxh64(&d) == (int64_t)0x8CB841DB40E6AE83ull);
    slang_slice_release(&d);
    slang_slice e = {0};
    assert(slang_compress_rt_crc32(0, &e) == 0);
    assert(slang_compress_rt_xxh64(&e) == (int64_t)0xEF46DB3751D8E999ull);
}

/* Inflates c in chunks of `chunk` bytes. */
static slang_slice inflate_chunked(const slang_slice *c, int64_t chunk, int *done) {
    slang_slice st, out = {0};
    slang_compress_rt_inflate_new(&st);
    for (int64_t i = 0; i < c->len; i += chunk) {
        int64_t k = c->len - i < chunk ? c->len - i : chunk;
        slang_slice in = bytes_of((uint8_t *)c->data + i, (size_t)k), part;
        slang_error *err = NULL;
        slang_compress_rt_inflate(&st, &in, &part, &err);
        assert(err == NULL);
        slang_slice_append_slice(&out, 1, NULL, &part);
        slang_slice_release(&part);
        slang_slice_release(&in);
    }
    *done = slang_compress_rt_inflate_done(&st);
    slang_slice_release(&st);
    return out;
}

TEST(compress_deflate_roundtrip) {
    static uint8_t buf[200000];
    for (size_t i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)("slang runtime deflate "[i % 22] ^ (i % 1000 == 0 ? 0x20 : 0));
    slang_slice data = bytes_of(buf, sizeof buf), dict = {0};
    for (int level = 0; level <= 9; level += 3) {
        slang_slice c;
        slang_compress_rt_deflate(&dict, &data, level, true, &c);
        assert(c.len > 0 && (level == 0 || c.len < 20000));
        int64_t chunks[] = {1, 100, 1 << 20};
        for (int k = 0; k < 3; k++) {
            if (chunks[k] == 1 && level != 6) continue;
            int done;
            slang_slice out = inflate_chunked(&c, chunks[k], &done);
            assert(done && out.len == data.len && memcmp(out.data, buf, sizeof buf) == 0);
            slang_slice_release(&out);
        }
        slang_slice_release(&c);
    }
    slang_slice_release(&data);
}

TEST(compress_inflate_tail_and_errors) {
    /* fixed block "abc" + 2 trailing bytes */
    slang_slice data = bytes_of("abc", 3), dict = {0}, c;
    slang_compress_rt_deflate(&dict, &data, 6, true, &c);
    uint8_t more[64];
    memcpy(more, c.data, (size_t)c.len);
    more[c.len] = 0xAA;
    more[c.len + 1] = 0xBB;
    slang_slice in = bytes_of(more, (size_t)c.len + 2), st, out, tail;
    slang_compress_rt_inflate_new(&st);
    slang_error *err = NULL;
    slang_compress_rt_inflate(&st, &in, &out, &err);
    assert(err == NULL && out.len == 3 && memcmp(out.data, "abc", 3) == 0);
    assert(slang_compress_rt_inflate_done(&st));
    slang_compress_rt_inflate_tail(&st, &tail);
    int64_t unused = slang_compress_rt_inflate_unused(&st);
    assert(tail.len + unused == 2);
    slang_slice_release(&tail);
    slang_slice_release(&out);
    slang_slice_release(&st);
    slang_slice_release(&in);
    slang_slice_release(&c);
    slang_slice_release(&data);
    uint8_t bad[] = {0x07};
    in = bytes_of(bad, 1);
    slang_compress_rt_inflate_new(&st);
    slang_compress_rt_inflate(&st, &in, &out, &err);
    assert(err != NULL && out.len == 0);
    slang_string *m = slang_error_message(err);
    assert(STR_IS(m, "deflate: invalid block type"));
    slang_release(m);
    slang_release(err);
    slang_slice_release(&st);
    slang_slice_release(&in);
}

TEST(compress_zstd) {
    static const uint8_t frame[] = {0x28, 0xb5, 0x2f, 0xfd, 0x20, 0x0a, 0x51, 0x00, 0x00, 'f', 'r', 'a', 'm', 'e', ' ', 't', 'w', 'o', '\n'};
    slang_slice in = bytes_of(frame, sizeof frame), out;
    slang_error *err = NULL;
    assert(slang_compress_rt_zstd_frame_len(&in) == (int64_t)sizeof frame);
    slang_compress_rt_zstd_decode(&in, &out, &err);
    assert(err == NULL && out.len == 10 && memcmp(out.data, "frame two\n", 10) == 0);
    slang_slice_release(&out);
    slang_slice_release(&in);
    in = bytes_of(frame, 12);
    assert(slang_compress_rt_zstd_frame_len(&in) == 0);
    slang_compress_rt_zstd_decode(&in, &out, &err);
    assert(err != NULL);
    slang_release(err);
    slang_slice_release(&in);
    in = bytes_of("nope", 4);
    assert(slang_compress_rt_zstd_frame_len(&in) == -1);
    slang_slice_release(&in);
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
