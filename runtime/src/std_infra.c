/* std_infra.c - C helpers of the tier-3b infrastructure clients (docs/STDLIB.md
 * "Уровень 3б"; runtime/ABI.md section 24): data.redis, data.kafka,
 * cloud.s3 and cloud.blob are Zell (compiler/src/sema/stdlib_src/
 * {redis*,kafka*,s3*,blob}.zl). Here: their immortal sentinel errors, an
 * amortized in-place byte append (Go's append(a, b...)), CRC16/XMODEM (Redis
 * Cluster key slots), CRC-32C (Kafka record batches) and murmur2 (the Kafka
 * Java client's default partitioner). */
#include "rt_internal.h"

#define INFRA_SENTINEL(fn, lit)                                                       \
    static ZELL_STRING_STORAGE(sizeof(lit)) fn##_msg = ZELL_STRING_INIT(fn##_msg, lit); \
    static zell_error fn##_err = { .hdr = { ZELL_RC_IMMORTAL, &zell_type_error },     \
                                    .message = (zell_string *)&fn##_msg };              \
    zell_error *fn(void) { return &fn##_err; }

/* data.redis */
INFRA_SENTINEL(zell_data_redis_err_nil, "redis: nil")
INFRA_SENTINEL(zell_data_redis_err_closed, "redis: client is closed")
INFRA_SENTINEL(zell_data_redis_err_tx_failed, "redis: transaction failed (a watched key changed)")
INFRA_SENTINEL(zell_data_redis_err_pool_timeout, "redis: connection pool timeout")
INFRA_SENTINEL(zell_data_redis_err_cross_slot, "redis: keys of the command map to different cluster slots")
/* data.kafka */
INFRA_SENTINEL(zell_data_kafka_err_closed, "kafka: client is closed")
INFRA_SENTINEL(zell_data_kafka_err_no_brokers, "kafka: no broker is reachable")
/* cloud.s3 */
INFRA_SENTINEL(zell_cloud_s3_err_no_such_key, "s3: NoSuchKey")
INFRA_SENTINEL(zell_cloud_s3_err_no_such_bucket, "s3: NoSuchBucket")
INFRA_SENTINEL(zell_cloud_s3_err_access_denied, "s3: AccessDenied")
INFRA_SENTINEL(zell_cloud_s3_err_precondition, "s3: PreconditionFailed")
/* cloud.blob */
INFRA_SENTINEL(zell_cloud_blob_err_not_found, "blob: not found")

/* append(a, b...) with Go's growth: out shares a's backing when it has room. */
void zell_data_rt_append(const zell_slice *a, const zell_slice *b, zell_slice *out) {
    *out = *a;
    zell_retain(out->backing);
    if (b && b->len > 0) zell_slice_append_slice(out, 1, NULL, b);
}

/* append(a, s...) for a string. */
void zell_data_rt_append_str(const zell_slice *a, const zell_string *s, zell_slice *out) {
    *out = *a;
    zell_retain(out->backing);
    int64_t n = s ? s->len : 0;
    if (n == 0) return;
    zell_slice tmp = { (void *)s->data, n, n, NULL };
    zell_slice_append_slice(out, 1, NULL, &tmp);
}

/* CRC16/XMODEM (polynomial 0x1021, init 0) of a string: Redis Cluster's
 * HASH_SLOT = crc16(key) mod 16384. */
int64_t zell_data_rt_crc16(const zell_string *s) {
    uint16_t crc = 0;
    int64_t n = s ? s->len : 0;
    for (int64_t i = 0; i < n; i++) {
        crc ^= (uint16_t)((uint8_t)s->data[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* CRC-32C (Castagnoli, reflected 0x82F63B78), slicing-by-8. crc continues
 * an earlier result (0 to start). */
static uint32_t crc32c_tab[8][256];
static int crc32c_ready;

static void crc32c_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        crc32c_tab[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++)
        for (int t = 1; t < 8; t++) crc32c_tab[t][i] = (crc32c_tab[t - 1][i] >> 8) ^ crc32c_tab[0][crc32c_tab[t - 1][i] & 0xff];
    __atomic_store_n(&crc32c_ready, 1, __ATOMIC_RELEASE);
}

uint32_t zell__crc32c(uint32_t crc, const uint8_t *p, size_t n) {
    if (!__atomic_load_n(&crc32c_ready, __ATOMIC_ACQUIRE)) crc32c_init();
    uint32_t c = ~crc;
    while (n >= 8) {
        uint32_t lo = c ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        c = crc32c_tab[7][lo & 0xff] ^ crc32c_tab[6][(lo >> 8) & 0xff] ^ crc32c_tab[5][(lo >> 16) & 0xff] ^
            crc32c_tab[4][lo >> 24] ^ crc32c_tab[3][p[4]] ^ crc32c_tab[2][p[5]] ^ crc32c_tab[1][p[6]] ^
            crc32c_tab[0][p[7]];
        p += 8;
        n -= 8;
    }
    while (n--) c = crc32c_tab[0][(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

int64_t zell_data_rt_crc32c(int64_t crc, const zell_slice *b) {
    int64_t n = b ? b->len : 0;
    return (int64_t)zell__crc32c((uint32_t)crc, n ? (const uint8_t *)b->data : NULL, (size_t)n);
}

/* murmur2 as in the Kafka Java client (Utils.murmur2: seed 0x9747b28c),
 * as a signed 32-bit value; the default partitioner takes
 * (murmur2 & 0x7fffffff) mod partitions. */
int64_t zell_data_rt_murmur2(const zell_slice *b) {
    const uint8_t *d = b && b->len ? (const uint8_t *)b->data : NULL;
    int64_t length = b ? b->len : 0;
    const uint32_t m = 0x5bd1e995u;
    const int r = 24;
    uint32_t h = 0x9747b28cu ^ (uint32_t)length;
    int64_t length4 = length / 4;
    for (int64_t i = 0; i < length4; i++) {
        int64_t i4 = i * 4;
        uint32_t k = (uint32_t)d[i4] | (uint32_t)d[i4 + 1] << 8 | (uint32_t)d[i4 + 2] << 16 | (uint32_t)d[i4 + 3] << 24;
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
    }
    switch (length % 4) {
    case 3: h ^= (uint32_t)d[(length & ~3) + 2] << 16; /* fall through */
    case 2: h ^= (uint32_t)d[(length & ~3) + 1] << 8;  /* fall through */
    case 1:
        h ^= (uint32_t)d[length & ~3];
        h *= m;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return (int64_t)(int32_t)h;
}
