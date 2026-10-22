/* Tests of std_http.c (runtime/ABI.md 16.5): head scanning and parsing,
 * canonical names, head formatting, tokens, dates, HPACK Huffman (RFC 7541
 * C.4 strings), WebSocket masking. */
#include "test.h"

zell_string *zell_net_http_rt_canonical(const zell_string *s);
int64_t zell_net_http_rt_head_end(const zell_slice *buf, int64_t from, int64_t to);
void zell_net_http_rt_parse_head(const zell_slice *buf, int64_t start, int64_t end, int64_t kind, zell_map *h,
                                  zell_slice *out, zell_error **err);
int64_t zell_net_http_rt_head_info(void);
zell_string *zell_net_http_rt_format_head(int64_t status, const zell_string *line, zell_map *h, int64_t blen,
                                            int64_t flags, const zell_string *body);
bool zell_net_http_rt_has_token(const zell_string *v, const zell_string *token);
zell_string *zell_net_http_rt_http_time(int64_t sec);
int64_t zell_net_http_rt_parse_http_time(const zell_string *s);
void zell_net_http_hpack_rt_huff_encode(const zell_string *s, zell_slice *out);
zell_string *zell_net_http_hpack_rt_huff_decode(const zell_slice *b, int64_t off, int64_t n, zell_error **err);
void zell_net_http_rt_mask(const zell_slice *b, int64_t off, int64_t n, int64_t key, int64_t phase);

static zell_slice bytes_of(const char *s) {
    zell_slice b;
    zell_string *t = S(s);
    zell_string_to_bytes(&b, t);
    zell_release(t);
    return b;
}

static zell_string *get(zell_map *h, const char *k) {
    zell_string *key = S(k);
    zell_string **v = zell_map_get_str(h, key);
    zell_release(key);
    return v ? *v : NULL;
}

TEST(test_canonical) {
    const char *cases[][2] = {{"content-type", "Content-Type"}, {"X-FOO-bar", "X-Foo-Bar"}, {" host ", "Host"},
                              {"Content-Type", "Content-Type"}, {"bad name", "bad name"}, {"x--y", "X--Y"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        zell_string *in = S(cases[i][0]);
        zell_string *out = zell_net_http_rt_canonical(in);
        assert(STR_IS(out, cases[i][1]));
        zell_release(out);
        zell_release(in);
    }
    /* already canonical: the argument itself */
    zell_string *in = S("X-Custom");
    zell_string *out = zell_net_http_rt_canonical(in);
    assert(out == in);
    zell_release(out);
    zell_release(in);
}

TEST(test_head_end) {
    zell_slice b = bytes_of("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY");
    assert(zell_net_http_rt_head_end(&b, 0, b.len) == 27);
    assert(zell_net_http_rt_head_end(&b, 0, 26) == -1);
    assert(zell_net_http_rt_head_end(&b, 20, b.len) == 27);
    zell_slice_release(&b);
    b = bytes_of("GET / HTTP/1.0\n\nx");
    assert(zell_net_http_rt_head_end(&b, 0, b.len) == 16);
    zell_slice_release(&b);
}

TEST(test_parse_request) {
    zell_slice b = bytes_of("POST /a/b?x=1&y HTTP/1.1\r\nhost: example\r\ncontent-length: 5\r\nX-A: 1\r\nx-a: 2\r\n"
                             "Set-Cookie: a=1\r\nset-cookie: b=2\r\nConnection: keep-alive, Close\r\n\r\nhello");
    int64_t end = zell_net_http_rt_head_end(&b, 0, b.len);
    zell_map *h = zell_map_new_str(8, &zell_ops_ref);
    zell_slice parts;
    zell_error *err = (zell_error *)1;
    zell_net_http_rt_parse_head(&b, 0, end, 0, h, &parts, &err);
    assert(err == NULL && parts.len == 4);
    zell_string **p = parts.data;
    assert(STR_IS(p[0], "POST") && STR_IS(p[1], "/a/b") && STR_IS(p[2], "x=1&y") && STR_IS(p[3], "HTTP/1.1"));
    assert(STR_IS(get(h, "Host"), "example") && STR_IS(get(h, "X-A"), "1, 2"));
    assert(STR_IS(get(h, "Set-Cookie"), "a=1\nb=2"));
    int64_t info = zell_net_http_rt_head_info();
    assert((info & 1) && (info >> 16) == 5 && (info & 8) && (info & 16) && !(info & 2));
    zell_slice_release(&parts);
    zell_release(h);
    zell_slice_release(&b);
    /* malformed */
    const char *bad[] = {"GET /\r\n\r\n", "GET / HTTP/2.0\r\n\r\n", "GET / HTTP/1.1\r\nNo colon\r\n\r\n",
                         "GET / HTTP/1.1\r\n folded: x\r\n\r\n", "GET / HTTP/1.1\r\nbad name: x\r\n\r\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        b = bytes_of(bad[i]);
        h = zell_map_new_str(8, &zell_ops_ref);
        zell_net_http_rt_parse_head(&b, 0, b.len, 0, h, &parts, &err);
        assert(err != NULL && parts.len == 0);
        zell_release(err);
        zell_release(h);
        zell_slice_release(&b);
    }
    /* conflicting Content-Length */
    b = bytes_of("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
    h = zell_map_new_str(8, &zell_ops_ref);
    zell_net_http_rt_parse_head(&b, 0, b.len, 0, h, &parts, &err);
    assert(err == NULL && (zell_net_http_rt_head_info() & 256));
    zell_slice_release(&parts);
    zell_release(h);
    zell_slice_release(&b);
    /* a response */
    b = bytes_of("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\n");
    h = zell_map_new_str(8, &zell_ops_ref);
    zell_net_http_rt_parse_head(&b, 0, b.len, 1, h, &parts, &err);
    p = parts.data;
    assert(err == NULL && STR_IS(p[0], "HTTP/1.1") && STR_IS(p[1], "404") && STR_IS(p[2], "Not Found"));
    zell_slice_release(&parts);
    zell_release(h);
    zell_slice_release(&b);
}

TEST(test_format_head) {
    zell_map *h = zell_map_new_str(8, &zell_ops_ref);
    zell_string *k = S("x-thing"), *v = S("a\r\nInjected: 1");
    zell_map_set_str(h, k, &v);
    zell_release(k);
    zell_release(v);
    zell_string *body = S("hello");
    zell_string *s = zell_net_http_rt_format_head(200, NULL, h, 5, 8 | 2, body);
    assert(STR_IS(s, "HTTP/1.1 200 OK\r\nX-Thing: a  Injected: 1\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: 5\r\nConnection: close\r\n\r\nhello"));
    zell_release(s);
    s = zell_net_http_rt_format_head(404, NULL, NULL, 0, 4 | 16, body);
    assert(strncmp(zell_string_data(s), "HTTP/1.1 404 Not Found\r\nDate: ", 30) == 0);
    assert(strstr(zell_string_data(s), "GMT\r\nTransfer-Encoding: chunked\r\n\r\n") != NULL);
    zell_release(s);
    zell_string *line = S("GET / HTTP/1.1\r\nHost: h");
    s = zell_net_http_rt_format_head(0, line, NULL, 0, 128 | 32, NULL);
    assert(STR_IS(s, "GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    zell_release(s);
    zell_release(line);
    zell_release(body);
    zell_release(h);
}

TEST(test_tokens_dates) {
    zell_string *v = S("gzip;q=1.0, Keep-Alive ,br"), *t1 = S("keep-alive"), *t2 = S("gzip"), *t3 = S("deflate");
    assert(zell_net_http_rt_has_token(v, t1) && zell_net_http_rt_has_token(v, t2) && !zell_net_http_rt_has_token(v, t3));
    zell_release(v);
    zell_release(t1);
    zell_release(t2);
    zell_release(t3);
    zell_string *d = zell_net_http_rt_http_time(784111777);
    assert(STR_IS(d, "Sun, 06 Nov 1994 08:49:37 GMT"));
    assert(zell_net_http_rt_parse_http_time(d) == 784111777);
    zell_release(d);
    const char *forms[] = {"Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994"};
    for (int i = 0; i < 2; i++) {
        d = S(forms[i]);
        assert(zell_net_http_rt_parse_http_time(d) == 784111777);
        zell_release(d);
    }
    d = S("yesterday");
    assert(zell_net_http_rt_parse_http_time(d) == -1);
    zell_release(d);
}

TEST(test_huffman) {
    /* RFC 7541 C.4.1 / C.4.2 */
    struct { const char *s; const unsigned char *hex; size_t n; } v[] = {
        {"www.example.com", (const unsigned char *)"\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 12},
        {"no-cache", (const unsigned char *)"\xa8\xeb\x10\x64\x9c\xbf", 6},
    };
    for (int i = 0; i < 2; i++) {
        zell_string *s = S(v[i].s);
        zell_slice enc;
        zell_net_http_hpack_rt_huff_encode(s, &enc);
        assert((size_t)enc.len == v[i].n && memcmp(enc.data, v[i].hex, v[i].n) == 0);
        zell_error *err;
        zell_string *dec = zell_net_http_hpack_rt_huff_decode(&enc, 0, enc.len, &err);
        assert(err == NULL && STR_IS(dec, v[i].s));
        zell_release(dec);
        zell_slice_release(&enc);
        zell_release(s);
    }
    /* every byte value round-trips */
    char all[256];
    for (int i = 0; i < 256; i++) all[i] = (char)i;
    zell_string *s = zell_string_new(all, 256);
    zell_slice enc;
    zell_net_http_hpack_rt_huff_encode(s, &enc);
    zell_error *err;
    zell_string *dec = zell_net_http_hpack_rt_huff_decode(&enc, 0, enc.len, &err);
    assert(err == NULL && zell_string_eq(dec, s));
    zell_release(dec);
    zell_slice_release(&enc);
    zell_release(s);
    /* the EOS symbol inside the data */
    zell_slice bad = bytes_of("\xff\xff\xff\xfc");
    dec = zell_net_http_hpack_rt_huff_decode(&bad, 0, bad.len, &err);
    assert(dec == NULL && err != NULL);
    zell_release(err);
    zell_slice_release(&bad);
    /* padding that is not all ones: 'a' (00011) + 000 */
    bad = bytes_of("\x18");
    dec = zell_net_http_hpack_rt_huff_decode(&bad, 0, bad.len, &err);
    assert(dec == NULL && err != NULL);
    zell_release(err);
    zell_slice_release(&bad);
}

TEST(test_mask) {
    zell_slice b = bytes_of("Hello, WebSocket masking!");
    zell_net_http_rt_mask(&b, 0, b.len, 0x37fa213d, 0);
    assert(((unsigned char *)b.data)[0] == ('H' ^ 0x37) && ((unsigned char *)b.data)[1] == ('e' ^ 0xfa));
    /* in two pieces with the phase carried over */
    zell_net_http_rt_mask(&b, 0, 3, 0x37fa213d, 0);
    zell_net_http_rt_mask(&b, 3, b.len - 3, 0x37fa213d, 3);
    assert(memcmp(b.data, "Hello, WebSocket masking!", (size_t)b.len) == 0);
    zell_slice_release(&b);
}

int main(void) {
    printf("std_http\n");
    RUN(test_canonical);
    RUN(test_head_end);
    RUN(test_parse_request);
    RUN(test_format_head);
    RUN(test_tokens_dates);
    RUN(test_huffman);
    RUN(test_mask);
    return 0;
}
