/* Tests of std_http.c (runtime/ABI.md 16.5): head scanning and parsing,
 * canonical names, head formatting, tokens, dates, HPACK Huffman (RFC 7541
 * C.4 strings), WebSocket masking. */
#include "test.h"

slang_string *slang_net_http_rt_canonical(const slang_string *s);
int64_t slang_net_http_rt_head_end(const slang_slice *buf, int64_t from, int64_t to);
void slang_net_http_rt_parse_head(const slang_slice *buf, int64_t start, int64_t end, int64_t kind, slang_map *h,
                                  slang_slice *out, slang_error **err);
int64_t slang_net_http_rt_head_info(void);
slang_string *slang_net_http_rt_format_head(int64_t status, const slang_string *line, slang_map *h, int64_t blen,
                                            int64_t flags, const slang_string *body);
bool slang_net_http_rt_has_token(const slang_string *v, const slang_string *token);
slang_string *slang_net_http_rt_http_time(int64_t sec);
int64_t slang_net_http_rt_parse_http_time(const slang_string *s);
void slang_net_http_hpack_rt_huff_encode(const slang_string *s, slang_slice *out);
slang_string *slang_net_http_hpack_rt_huff_decode(const slang_slice *b, int64_t off, int64_t n, slang_error **err);
void slang_net_http_rt_mask(const slang_slice *b, int64_t off, int64_t n, int64_t key, int64_t phase);

static slang_slice bytes_of(const char *s) {
    slang_slice b;
    slang_string *t = S(s);
    slang_string_to_bytes(&b, t);
    slang_release(t);
    return b;
}

static slang_string *get(slang_map *h, const char *k) {
    slang_string *key = S(k);
    slang_string **v = slang_map_get_str(h, key);
    slang_release(key);
    return v ? *v : NULL;
}

TEST(test_canonical) {
    const char *cases[][2] = {{"content-type", "Content-Type"}, {"X-FOO-bar", "X-Foo-Bar"}, {" host ", "Host"},
                              {"Content-Type", "Content-Type"}, {"bad name", "bad name"}, {"x--y", "X--Y"}};
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        slang_string *in = S(cases[i][0]);
        slang_string *out = slang_net_http_rt_canonical(in);
        assert(STR_IS(out, cases[i][1]));
        slang_release(out);
        slang_release(in);
    }
    /* already canonical: the argument itself */
    slang_string *in = S("X-Custom");
    slang_string *out = slang_net_http_rt_canonical(in);
    assert(out == in);
    slang_release(out);
    slang_release(in);
}

TEST(test_head_end) {
    slang_slice b = bytes_of("GET / HTTP/1.1\r\nHost: x\r\n\r\nBODY");
    assert(slang_net_http_rt_head_end(&b, 0, b.len) == 27);
    assert(slang_net_http_rt_head_end(&b, 0, 26) == -1);
    assert(slang_net_http_rt_head_end(&b, 20, b.len) == 27);
    slang_slice_release(&b);
    b = bytes_of("GET / HTTP/1.0\n\nx");
    assert(slang_net_http_rt_head_end(&b, 0, b.len) == 16);
    slang_slice_release(&b);
}

TEST(test_parse_request) {
    slang_slice b = bytes_of("POST /a/b?x=1&y HTTP/1.1\r\nhost: example\r\ncontent-length: 5\r\nX-A: 1\r\nx-a: 2\r\n"
                             "Set-Cookie: a=1\r\nset-cookie: b=2\r\nConnection: keep-alive, Close\r\n\r\nhello");
    int64_t end = slang_net_http_rt_head_end(&b, 0, b.len);
    slang_map *h = slang_map_new_str(8, &slang_ops_ref);
    slang_slice parts;
    slang_error *err = (slang_error *)1;
    slang_net_http_rt_parse_head(&b, 0, end, 0, h, &parts, &err);
    assert(err == NULL && parts.len == 4);
    slang_string **p = parts.data;
    assert(STR_IS(p[0], "POST") && STR_IS(p[1], "/a/b") && STR_IS(p[2], "x=1&y") && STR_IS(p[3], "HTTP/1.1"));
    assert(STR_IS(get(h, "Host"), "example") && STR_IS(get(h, "X-A"), "1, 2"));
    assert(STR_IS(get(h, "Set-Cookie"), "a=1\nb=2"));
    int64_t info = slang_net_http_rt_head_info();
    assert((info & 1) && (info >> 16) == 5 && (info & 8) && (info & 16) && !(info & 2));
    slang_slice_release(&parts);
    slang_release(h);
    slang_slice_release(&b);
    /* malformed */
    const char *bad[] = {"GET /\r\n\r\n", "GET / HTTP/2.0\r\n\r\n", "GET / HTTP/1.1\r\nNo colon\r\n\r\n",
                         "GET / HTTP/1.1\r\n folded: x\r\n\r\n", "GET / HTTP/1.1\r\nbad name: x\r\n\r\n"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        b = bytes_of(bad[i]);
        h = slang_map_new_str(8, &slang_ops_ref);
        slang_net_http_rt_parse_head(&b, 0, b.len, 0, h, &parts, &err);
        assert(err != NULL && parts.len == 0);
        slang_release(err);
        slang_release(h);
        slang_slice_release(&b);
    }
    /* conflicting Content-Length */
    b = bytes_of("GET / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
    h = slang_map_new_str(8, &slang_ops_ref);
    slang_net_http_rt_parse_head(&b, 0, b.len, 0, h, &parts, &err);
    assert(err == NULL && (slang_net_http_rt_head_info() & 256));
    slang_slice_release(&parts);
    slang_release(h);
    slang_slice_release(&b);
    /* a response */
    b = bytes_of("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\n");
    h = slang_map_new_str(8, &slang_ops_ref);
    slang_net_http_rt_parse_head(&b, 0, b.len, 1, h, &parts, &err);
    p = parts.data;
    assert(err == NULL && STR_IS(p[0], "HTTP/1.1") && STR_IS(p[1], "404") && STR_IS(p[2], "Not Found"));
    slang_slice_release(&parts);
    slang_release(h);
    slang_slice_release(&b);
}

TEST(test_format_head) {
    slang_map *h = slang_map_new_str(8, &slang_ops_ref);
    slang_string *k = S("x-thing"), *v = S("a\r\nInjected: 1");
    slang_map_set_str(h, k, &v);
    slang_release(k);
    slang_release(v);
    slang_string *body = S("hello");
    slang_string *s = slang_net_http_rt_format_head(200, NULL, h, 5, 8 | 2, body);
    assert(STR_IS(s, "HTTP/1.1 200 OK\r\nX-Thing: a  Injected: 1\r\nContent-Type: text/plain; charset=utf-8\r\n"
                     "Content-Length: 5\r\nConnection: close\r\n\r\nhello"));
    slang_release(s);
    s = slang_net_http_rt_format_head(404, NULL, NULL, 0, 4 | 16, body);
    assert(strncmp(slang_string_data(s), "HTTP/1.1 404 Not Found\r\nDate: ", 30) == 0);
    assert(strstr(slang_string_data(s), "GMT\r\nTransfer-Encoding: chunked\r\n\r\n") != NULL);
    slang_release(s);
    slang_string *line = S("GET / HTTP/1.1\r\nHost: h");
    s = slang_net_http_rt_format_head(0, line, NULL, 0, 128 | 32, NULL);
    assert(STR_IS(s, "GET / HTTP/1.1\r\nHost: h\r\n\r\n"));
    slang_release(s);
    slang_release(line);
    slang_release(body);
    slang_release(h);
}

TEST(test_tokens_dates) {
    slang_string *v = S("gzip;q=1.0, Keep-Alive ,br"), *t1 = S("keep-alive"), *t2 = S("gzip"), *t3 = S("deflate");
    assert(slang_net_http_rt_has_token(v, t1) && slang_net_http_rt_has_token(v, t2) && !slang_net_http_rt_has_token(v, t3));
    slang_release(v);
    slang_release(t1);
    slang_release(t2);
    slang_release(t3);
    slang_string *d = slang_net_http_rt_http_time(784111777);
    assert(STR_IS(d, "Sun, 06 Nov 1994 08:49:37 GMT"));
    assert(slang_net_http_rt_parse_http_time(d) == 784111777);
    slang_release(d);
    const char *forms[] = {"Sunday, 06-Nov-94 08:49:37 GMT", "Sun Nov  6 08:49:37 1994"};
    for (int i = 0; i < 2; i++) {
        d = S(forms[i]);
        assert(slang_net_http_rt_parse_http_time(d) == 784111777);
        slang_release(d);
    }
    d = S("yesterday");
    assert(slang_net_http_rt_parse_http_time(d) == -1);
    slang_release(d);
}

TEST(test_huffman) {
    /* RFC 7541 C.4.1 / C.4.2 */
    struct { const char *s; const unsigned char *hex; size_t n; } v[] = {
        {"www.example.com", (const unsigned char *)"\xf1\xe3\xc2\xe5\xf2\x3a\x6b\xa0\xab\x90\xf4\xff", 12},
        {"no-cache", (const unsigned char *)"\xa8\xeb\x10\x64\x9c\xbf", 6},
    };
    for (int i = 0; i < 2; i++) {
        slang_string *s = S(v[i].s);
        slang_slice enc;
        slang_net_http_hpack_rt_huff_encode(s, &enc);
        assert((size_t)enc.len == v[i].n && memcmp(enc.data, v[i].hex, v[i].n) == 0);
        slang_error *err;
        slang_string *dec = slang_net_http_hpack_rt_huff_decode(&enc, 0, enc.len, &err);
        assert(err == NULL && STR_IS(dec, v[i].s));
        slang_release(dec);
        slang_slice_release(&enc);
        slang_release(s);
    }
    /* every byte value round-trips */
    char all[256];
    for (int i = 0; i < 256; i++) all[i] = (char)i;
    slang_string *s = slang_string_new(all, 256);
    slang_slice enc;
    slang_net_http_hpack_rt_huff_encode(s, &enc);
    slang_error *err;
    slang_string *dec = slang_net_http_hpack_rt_huff_decode(&enc, 0, enc.len, &err);
    assert(err == NULL && slang_string_eq(dec, s));
    slang_release(dec);
    slang_slice_release(&enc);
    slang_release(s);
    /* the EOS symbol inside the data */
    slang_slice bad = bytes_of("\xff\xff\xff\xfc");
    dec = slang_net_http_hpack_rt_huff_decode(&bad, 0, bad.len, &err);
    assert(dec == NULL && err != NULL);
    slang_release(err);
    slang_slice_release(&bad);
    /* padding that is not all ones: 'a' (00011) + 000 */
    bad = bytes_of("\x18");
    dec = slang_net_http_hpack_rt_huff_decode(&bad, 0, bad.len, &err);
    assert(dec == NULL && err != NULL);
    slang_release(err);
    slang_slice_release(&bad);
}

TEST(test_mask) {
    slang_slice b = bytes_of("Hello, WebSocket masking!");
    slang_net_http_rt_mask(&b, 0, b.len, 0x37fa213d, 0);
    assert(((unsigned char *)b.data)[0] == ('H' ^ 0x37) && ((unsigned char *)b.data)[1] == ('e' ^ 0xfa));
    /* in two pieces with the phase carried over */
    slang_net_http_rt_mask(&b, 0, 3, 0x37fa213d, 0);
    slang_net_http_rt_mask(&b, 3, b.len - 3, 0x37fa213d, 3);
    assert(memcmp(b.data, "Hello, WebSocket masking!", (size_t)b.len) == 0);
    slang_slice_release(&b);
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
