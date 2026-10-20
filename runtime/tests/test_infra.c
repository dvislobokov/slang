/* Tests of std_infra.c (runtime/ABI.md section 24): CRC-32C (RFC 3720 B.4 and
 * the Kafka Java client's Crc32C), CRC16/XMODEM key slots as Redis Cluster
 * computes them, murmur2 vectors from the Kafka Java client (Utils.murmur2,
 * checked against kafka-clients 3.7.0), the in-place byte append and the
 * sentinels. */
#include "test.h"

int64_t slang_data_rt_crc32c(int64_t crc, const slang_slice *b);
int64_t slang_data_rt_crc16(const slang_string *s);
int64_t slang_data_rt_murmur2(const slang_slice *b);
void slang_data_rt_append(const slang_slice *a, const slang_slice *b, slang_slice *out);
void slang_data_rt_append_str(const slang_slice *a, const slang_string *s, slang_slice *out);
slang_error *slang_data_redis_err_nil(void);
slang_error *slang_cloud_s3_err_no_such_key(void);

static slang_slice bytes_of(const char *s) {
    slang_slice b;
    slang_string *t = S(s);
    slang_string_to_bytes(&b, t);
    slang_release(t);
    return b;
}

TEST(test_crc32c) {
    slang_slice b = bytes_of("123456789");
    assert(slang_data_rt_crc32c(0, &b) == 0xE3069283);
    /* incremental, across the 8-byte stride */
    slang_slice lo = { b.data, 5, 5, NULL }, hi = { (char *)b.data + 5, 4, 4, NULL };
    assert(slang_data_rt_crc32c(slang_data_rt_crc32c(0, &lo), &hi) == 0xE3069283);
    slang_slice_release(&b);
    /* RFC 3720 B.4: 32 bytes of zeros / of 0xff / ascending */
    uint8_t z[32], f[32], a[32];
    for (int i = 0; i < 32; i++) z[i] = 0, f[i] = 0xff, a[i] = (uint8_t)i;
    slang_slice sz = { z, 32, 32, NULL }, sf = { f, 32, 32, NULL }, sa = { a, 32, 32, NULL };
    assert(slang_data_rt_crc32c(0, &sz) == 0x8A9136AA);
    assert(slang_data_rt_crc32c(0, &sf) == 0x62A8AB43);
    assert(slang_data_rt_crc32c(0, &sa) == 0x46DD794E);
    slang_slice empty = { 0 };
    assert(slang_data_rt_crc32c(0, &empty) == 0);
}

TEST(test_crc16_slots) {
    slang_string *s = S("123456789");
    assert(slang_data_rt_crc16(s) == 0x31C3);
    slang_release(s);
    /* CLUSTER KEYSLOT foo / bar / hello */
    const char *keys[] = { "foo", "bar", "hello" };
    const int64_t slots[] = { 12182, 5061, 866 };
    for (int i = 0; i < 3; i++) {
        slang_string *k = S(keys[i]);
        assert(slang_data_rt_crc16(k) % 16384 == slots[i]);
        slang_release(k);
    }
}

TEST(test_murmur2) {
    const char *in[] = { "21", "foobar", "a-little-bit-long-string", "a-little-bit-longer-string",
                         "lkjh234lh9fiuh90y23oiuhsafujhadof229phr9h19h89h8", "abc", "" };
    const int64_t want[] = { -973932308, -790332482, -985981536, -1486304829, -58897971, 479470107, 275646681 };
    for (int i = 0; i < 7; i++) {
        slang_slice b = bytes_of(in[i]);
        assert(slang_data_rt_murmur2(&b) == want[i]);
        slang_slice_release(&b);
    }
}

TEST(test_append) {
    slang_slice a = { 0 }, out;
    slang_slice x = bytes_of("hello ");
    slang_data_rt_append(&a, &x, &out);
    assert(out.len == 6);
    slang_string *w = S("world");
    slang_slice out2;
    slang_data_rt_append_str(&out, w, &out2);
    assert(out2.len == 11 && memcmp(out2.data, "hello world", 11) == 0);
    /* amortized growth: appending many small pieces reuses the backing */
    slang_slice acc = out2, next;
    slang_retain(acc.backing);
    for (int i = 0; i < 1000; i++) {
        slang_data_rt_append_str(&acc, w, &next);
        slang_slice_release(&acc);
        acc = next;
    }
    assert(acc.len == 11 + 5000 && acc.cap >= acc.len);
    slang_slice_release(&acc);
    slang_release(w);
    slang_slice_release(&out2);
    slang_slice_release(&out);
    slang_slice_release(&x);
}

TEST(test_sentinels) {
    assert(STR_IS(slang_data_redis_err_nil()->message, "redis: nil"));
    assert(STR_IS(slang_cloud_s3_err_no_such_key()->message, "s3: NoSuchKey"));
    assert(slang_data_redis_err_nil() == slang_data_redis_err_nil());
}

int main(void) {
    printf("std_infra\n");
    RUN(test_crc32c);
    RUN(test_crc16_slots);
    RUN(test_murmur2);
    RUN(test_append);
    RUN(test_sentinels);
    return 0;
}
