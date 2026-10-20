/* Tests for the C halves of the tier-3 stdlib (docs/STDLIB.md level 3,
 * runtime/ABI.md 23): the zstd encoder (round trips through our decoder
 * and, when installed, the zstd CLI), Argon2 (RFC 9106 vectors), BLAKE2b,
 * PBKDF2 (RFC 6070 / RFC 7914), scrypt (RFC 7914), P-384 (RFC 6979 A.2.6),
 * AES-CBC (SP 800-38A F.2) with PKCS#7, RSA-OAEP and prime generation, and
 * the std.database.sql sentinels. Prototypes are declared here: the
 * functions are private to their Slang namespaces. */
#include "test.h"

void slang_compress_rt_zstd_encode(const slang_slice *data, int64_t level, bool checksum, slang_slice *out, slang_error **err);
void slang_compress_rt_zstd_decode(const slang_slice *data, slang_slice *out, slang_error **err);

void slang_crypto_kdf_rt_argon2(int64_t type, const slang_slice *pw, const slang_slice *salt, const slang_slice *secret,
                                const slang_slice *ad, int64_t t, int64_t m, int64_t p, int64_t n, slang_slice *out,
                                slang_error **err);
void slang_crypto_kdf_pbkdf2(const slang_slice *pw, const slang_slice *salt, int64_t iter, int64_t n,
                             const slang_string *hash, slang_slice *out, slang_error **err);
void slang_crypto_kdf_scrypt(const slang_slice *pw, const slang_slice *salt, int64_t N, int64_t r, int64_t p,
                             int64_t n, slang_slice *out, slang_error **err);
void slang_crypto_hash_blake2b(const slang_slice *data, int64_t size, const slang_slice *key, slang_slice *out,
                               slang_error **err);
void slang_crypto_ecdsa_public_key_p384(const slang_slice *priv, slang_slice *out, slang_error **err);
void slang_crypto_ecdsa_sign_p384_raw(const slang_slice *priv, const slang_slice *digest, slang_slice *out,
                                         slang_error **err);
void slang_crypto_ecdsa_sign_p384(const slang_slice *priv, const slang_slice *digest, slang_slice *out, slang_error **err);
bool slang_crypto_ecdsa_verify_p384(const slang_slice *pub, const slang_slice *digest, const slang_slice *sig);
void slang_crypto_ecdh_shared_secret_p384(const slang_slice *priv, const slang_slice *peer, slang_slice *out,
                                          slang_error **err);
void slang_crypto_aes_cbc_encrypt(const slang_slice *key, const slang_slice *iv, const slang_slice *pt, slang_slice *out,
                                  slang_error **err);
void slang_crypto_aes_cbc_decrypt(const slang_slice *key, const slang_slice *iv, const slang_slice *ct, slang_slice *out,
                                  slang_error **err);
void slang_crypto_aes_cbc_encrypt_raw(const slang_slice *key, const slang_slice *iv, const slang_slice *pt,
                                      slang_slice *out, slang_error **err);
void slang_crypto_aes_decrypt_block(const slang_slice *key, const slang_slice *block, slang_slice *out,
                                    slang_error **err);
void slang_crypto_rsa_rt_gen_prime(int64_t bits, int64_t e, slang_slice *out, slang_error **err);
bool slang_crypto_rsa_is_probable_prime(const slang_slice *p, int64_t rounds);
slang_error *slang_database_sql_err_no_rows(void);
void slang_compress_rt_brotli_decode(const slang_slice *data, slang_slice *out, slang_error **err);

static slang_slice bytes(const void *p, size_t n) {
    slang_slice s;
    slang_slice_from(&s, 1, NULL, p, (int64_t)n);
    return s;
}

static slang_slice rep(int b, size_t n) {
    uint8_t buf[64];
    memset(buf, b, sizeof buf);
    return bytes(buf, n);
}

static int hexval(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }

static slang_slice unhex(const char *h) {
    size_t n = strlen(h) / 2;
    uint8_t *b = malloc(n + 1);
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(hexval(h[2 * i]) << 4 | hexval(h[2 * i + 1]));
    slang_slice s = bytes(b, n);
    free(b);
    return s;
}

static int is_hex(const slang_slice *s, const char *h) {
    if ((size_t)s->len * 2 != strlen(h)) return 0;
    static const char d[] = "0123456789abcdef";
    const uint8_t *p = s->data;
    for (int64_t i = 0; i < s->len; i++)
        if (d[p[i] >> 4] != h[2 * i] || d[p[i] & 15] != h[2 * i + 1]) return 0;
    return 1;
}

/* ---- zstd ---- */

static uint64_t rng = 88172645463325252ull;
static uint32_t xr(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)rng;
}

static void zstd_roundtrip(const uint8_t *p, size_t n, int level, int cli) {
    slang_slice in = bytes(p, n), enc, dec;
    slang_error *err = NULL;
    slang_compress_rt_zstd_encode(&in, level, true, &enc, &err);
    assert(!err);
    slang_compress_rt_zstd_decode(&enc, &dec, &err);
    if (err) fprintf(stderr, "decode failed (n=%zu level=%d): %s\n", n, level, slang_string_data(err->message));
    assert(!err);
    assert(dec.len == (int64_t)n && (n == 0 || memcmp(dec.data, p, n) == 0));
    if (cli) {
        /* the reference decoder must accept the frame too */
        char path[] = "/tmp/slang_zstd_XXXXXX", orig[] = "/tmp/slang_zorig_XXXXXX";
        int fd = mkstemp(path), fo = mkstemp(orig);
        assert(fd >= 0 && fo >= 0);
        assert(write(fd, enc.data, (size_t)enc.len) == (ssize_t)enc.len);
        assert(n == 0 || write(fo, p, n) == (ssize_t)n);
        close(fd);
        close(fo);
        char cmd[256];
        snprintf(cmd, sizeof cmd, "zstd -q -d -c %s 2>/dev/null | cmp -s - %s", path, orig);
        int st = system(cmd);
        if (st != 0) fprintf(stderr, "zstd CLI rejected the frame (n=%zu level=%d)\n", n, level);
        assert(st == 0);
        unlink(path);
        unlink(orig);
    }
    slang_slice_release(&in);
    slang_slice_release(&enc);
    slang_slice_release(&dec);
}

TEST(zstd_encoder) {
    int cli = system("zstd --version >/dev/null 2>&1") == 0;
    if (!cli) printf("  (zstd CLI not found: checked with our decoder only)\n");
    size_t sizes[] = { 0, 1, 5, 31, 64, 100, 1000, 4096, 70000, 131072, 131073, 300000 };
    uint8_t *buf = malloc(9u << 20);
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        size_t n = sizes[k];
        /* text-like: words from a small vocabulary */
        static const char *words[] = { "the ", "quick ", "brown ", "fox ", "jumps ", "over ", "lazy ", "dog ", "\n" };
        size_t i = 0;
        while (i < n) {
            const char *w = words[xr() % 9];
            for (size_t j = 0; w[j] && i < n; j++) buf[i++] = (uint8_t)w[j];
        }
        for (int level = 1; level <= 9; level += 4) zstd_roundtrip(buf, n, level, cli && level != 5);
        /* random bytes (raw blocks / raw literals) */
        for (i = 0; i < n; i++) buf[i] = (uint8_t)xr();
        zstd_roundtrip(buf, n, 3, cli);
        /* skewed binary over all 256 values (FSE-compressed Huffman weights) */
        for (i = 0; i < n; i++) {
            uint32_t r = xr();
            buf[i] = (uint8_t)(r % 7 == 0 ? r >> 8 : (r >> 8) % 3 + 200);
        }
        zstd_roundtrip(buf, n, 3, cli);
        /* runs (RLE blocks and literals) */
        memset(buf, 'a', n);
        zstd_roundtrip(buf, n, 1, cli);
    }
    /* larger than the single-segment limit: 8 MiB window */
    size_t big = (8u << 20) + 12345;
    for (size_t i = 0; i < big; i++) buf[i] = (uint8_t)("abcdefgh"[xr() % 8]);
    zstd_roundtrip(buf, big, 1, cli);
    /* compression actually happens */
    memset(buf, 0, 100000);
    for (size_t i = 0; i < 100000; i++) buf[i] = (uint8_t)("hello world, "[i % 13]);
    slang_slice in = bytes(buf, 100000), enc;
    slang_error *err = NULL;
    slang_compress_rt_zstd_encode(&in, 3, false, &enc, &err);
    assert(!err && enc.len < 1000);
    slang_slice_release(&in);
    slang_slice_release(&enc);
    free(buf);
}

/* ---- KDFs ---- */

TEST(argon2_rfc9106) {
    slang_slice pw = rep(1, 32), salt = rep(2, 16), secret = rep(3, 8), ad = rep(4, 12), out;
    slang_error *err = NULL;
    static const char *want[3] = {
        "512b391b6f1162975371d30919734294f868e3be3984f3c1a13a4db9fabe4acb",
        "c814d9d1dc7f37aa13f0d77f2494bda1c8de6b016dd388d29952a4c4672b6ce8",
        "0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659",
    };
    for (int t = 0; t < 3; t++) {
        slang_crypto_kdf_rt_argon2(t, &pw, &salt, &secret, &ad, 3, 32, 4, 32, &out, &err);
        assert(!err && is_hex(&out, want[t]));
        slang_slice_release(&out);
    }
    /* parameter errors */
    slang_crypto_kdf_rt_argon2(2, &pw, &salt, NULL, NULL, 0, 32, 4, 32, &out, &err);
    assert(err && out.len == 0);
    slang_release(err);
    err = NULL;
    slang_slice shortsalt = rep(2, 4);
    slang_crypto_kdf_rt_argon2(2, &pw, &shortsalt, NULL, NULL, 1, 32, 1, 32, &out, &err);
    assert(err);
    slang_release(err);
    slang_slice_release(&shortsalt);
    slang_slice_release(&pw);
    slang_slice_release(&salt);
    slang_slice_release(&secret);
    slang_slice_release(&ad);
}

TEST(pbkdf2_scrypt_blake2b) {
    slang_slice pw = bytes("password", 8), salt = bytes("salt", 4), out;
    slang_error *err = NULL;
    slang_string *sha1 = S("sha1"), *sha256 = S("sha256");
    slang_crypto_kdf_pbkdf2(&pw, &salt, 1, 20, sha1, &out, &err);
    assert(!err && is_hex(&out, "0c60c80f961f0e71f3a9b524af6012062fe037a6"));
    slang_slice_release(&out);
    slang_crypto_kdf_pbkdf2(&pw, &salt, 4096, 20, sha1, &out, &err);
    assert(!err && is_hex(&out, "4b007901b765489abead49d926f721d065a429c1"));
    slang_slice_release(&out);
    /* RFC 7914 11: PBKDF2-HMAC-SHA256 P="passwd", S="salt", c=1, dkLen=64 */
    slang_slice pw2 = bytes("passwd", 6);
    slang_crypto_kdf_pbkdf2(&pw2, &salt, 1, 64, sha256, &out, &err);
    assert(!err && is_hex(&out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783"));
    slang_slice_release(&out);
    /* RFC 7914 12: scrypt("password", "NaCl", N=1024, r=8, p=16, 64) */
    slang_slice nacl = bytes("NaCl", 4);
    slang_crypto_kdf_scrypt(&pw, &nacl, 1024, 8, 16, 64, &out, &err);
    assert(!err && is_hex(&out, "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640"));
    slang_slice_release(&out);
    slang_crypto_kdf_scrypt(&pw, &nacl, 1000, 8, 1, 64, &out, &err);
    assert(err);
    slang_release(err);
    err = NULL;
    /* RFC 7693 A: BLAKE2b-512("abc") */
    slang_slice abc = bytes("abc", 3);
    slang_crypto_hash_blake2b(&abc, 64, NULL, &out, &err);
    assert(!err && is_hex(&out, "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923"));
    slang_slice_release(&out);
    slang_release(sha1);
    slang_release(sha256);
    slang_slice_release(&pw);
    slang_slice_release(&pw2);
    slang_slice_release(&salt);
    slang_slice_release(&nacl);
    slang_slice_release(&abc);
}

/* ---- P-384, AES-CBC, RSA ---- */

TEST(p384_rfc6979) {
    slang_slice priv = unhex("6b9d3dad2e1b8c1c05b19875b6659f4de23c3b667bf297ba9aa47740787137d896d5724e4c70a825f872c9ea60d2edf5");
    /* SHA-384("sample") */
    slang_slice digest = unhex("9a9083505bc92276aec4be312696ef7bf3bf603f4bbd381196a029f340585312313bca4a9b5b890efee42c77b1ee25fe");
    slang_slice pub, sig, der;
    slang_error *err = NULL;
    slang_crypto_ecdsa_public_key_p384(&priv, &pub, &err);
    assert(!err && is_hex(&pub, "04ec3a4e415b4e19a4568618029f427fa5da9a8bc4ae92e02e06aae5286b300c64def8f0ea9055866064a254515480bc138015d9b72d7d57244ea8ef9ac0c621896708a59367f9dfb9f54ca84b3f1c9db1288b231c3ae0d4fe7344fd2533264720"));
    slang_crypto_ecdsa_sign_p384_raw(&priv, &digest, &sig, &err);
    assert(!err && is_hex(&sig, "94edbb92a5ecb8aad4736e56c691916b3f88140666ce9fa73d64c4ea95ad133c81a648152e44acf96e36dd1e80fabe4699ef4aeb15f178cea1fe40db2603138f130e740a19624526203b6351d0a3a94fa329c145786e679e7b82c71a38628ac8"));
    slang_crypto_ecdsa_sign_p384(&priv, &digest, &der, &err);
    assert(!err && slang_crypto_ecdsa_verify_p384(&pub, &digest, &der));
    ((uint8_t *)digest.data)[0] ^= 1;
    assert(!slang_crypto_ecdsa_verify_p384(&pub, &digest, &der));
    /* ECDH against a vector computed with OpenSSL (python cryptography) */
    slang_slice a = unhex("543f88e5b664c8cfae8f912341a7fbceefc525f32d4bc5cad29f68484096c21e8323c517094ddf365996b97a73eca1b1");
    slang_slice bpub = unhex("04cea60335e06f588eb2d17f995a05cc9207f7b6aa42649900dc0edacb0f4806297ea6a4c64d414f9fda13fa8e70c2e71d8ccd244e4e093028db0a5eea7dad6164f3d13dd6409a16fd8393365f18d92689103b8d0f88d298ac9224436c3c3ff139");
    slang_slice shared;
    slang_crypto_ecdh_shared_secret_p384(&a, &bpub, &shared, &err);
    assert(!err && is_hex(&shared, "eba84ea917fa14c9c21df9ac192af27df0f378c5ea49c0853bfe3d41c4a3c9fc180aadb1da2117fea2872a73a1ca316a"));
    ((uint8_t *)bpub.data)[60] ^= 1;   /* off the curve */
    slang_slice bad;
    slang_crypto_ecdh_shared_secret_p384(&a, &bpub, &bad, &err);
    assert(err);
    slang_release(err);
    slang_slice_release(&priv);
    slang_slice_release(&digest);
    slang_slice_release(&pub);
    slang_slice_release(&sig);
    slang_slice_release(&der);
    slang_slice_release(&a);
    slang_slice_release(&bpub);
    slang_slice_release(&shared);
}

TEST(aes_cbc) {
    slang_slice key = unhex("2b7e151628aed2a6abf7158809cf4f3c"), iv = unhex("000102030405060708090a0b0c0d0e0f");
    slang_slice pt = unhex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    slang_slice ct, back;
    slang_error *err = NULL;
    slang_crypto_aes_cbc_encrypt_raw(&key, &iv, &pt, &ct, &err);
    assert(!err && is_hex(&ct, "7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b273bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7"));
    slang_slice_release(&ct);
    /* AES-256 F.2.5 */
    slang_slice key256 = unhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
    slang_crypto_aes_cbc_encrypt_raw(&key256, &iv, &pt, &ct, &err);
    assert(!err && is_hex(&ct, "f58c4c04d6e5f1ba779eabfb5f7bfbd69cfc4e967edb808d679f777bc6702c7d39f23369a9d9bacfa530e26304231461b2eb05e2c39be9fcda6c19078c6a9d1b"));
    slang_slice blk = { 0 }, dec;
    slang_slice_sub(&blk, &ct, 0, 16, 1);
    slang_crypto_aes_decrypt_block(&key256, &blk, &dec, &err);
    assert(!err);
    for (int i = 0; i < 16; i++) assert((((uint8_t *)dec.data)[i] ^ ((uint8_t *)iv.data)[i]) == ((uint8_t *)pt.data)[i]);
    slang_slice_release(&blk);
    slang_slice_release(&dec);
    slang_slice_release(&ct);
    /* padded round trips of every length 0..40 */
    for (int n = 0; n <= 40; n++) {
        slang_slice m = rep('x', (size_t)n);
        slang_crypto_aes_cbc_encrypt(&key, &iv, &m, &ct, &err);
        assert(!err && ct.len == (n / 16 + 1) * 16);
        slang_crypto_aes_cbc_decrypt(&key, &iv, &ct, &back, &err);
        assert(!err && back.len == n && (n == 0 || memcmp(back.data, m.data, (size_t)n) == 0));
        ((uint8_t *)ct.data)[ct.len - 1] ^= 0x55;   /* corrupt the padding */
        slang_slice bad;
        slang_crypto_aes_cbc_decrypt(&key, &iv, &ct, &bad, &err);
        assert(err && strstr(slang_string_data(err->message), "padding"));
        slang_release(err);
        err = NULL;
        slang_slice_release(&m);
        slang_slice_release(&ct);
        slang_slice_release(&back);
    }
    slang_slice_release(&key);
    slang_slice_release(&key256);
    slang_slice_release(&iv);
    slang_slice_release(&pt);
}

TEST(rsa_primes) {
    slang_slice p;
    slang_error *err = NULL;
    slang_crypto_rsa_rt_gen_prime(512, 65537, &p, &err);
    assert(!err && p.len == 64 && (((uint8_t *)p.data)[0] & 0xC0) == 0xC0 && (((uint8_t *)p.data)[63] & 1));
    assert(slang_crypto_rsa_is_probable_prime(&p, 20));
    slang_slice_release(&p);
    /* 2^127 - 1 is prime, 2^128 + 1 is not; the Carmichael number 561 is not */
    slang_slice m127 = unhex("7fffffffffffffffffffffffffffffff"), f7 = unhex("0100000000000000000000000000000001"), c561 = unhex("0231");
    slang_slice c97 = unhex("61");
    assert(slang_crypto_rsa_is_probable_prime(&m127, 20));
    assert(!slang_crypto_rsa_is_probable_prime(&f7, 20));
    assert(!slang_crypto_rsa_is_probable_prime(&c561, 20));
    assert(slang_crypto_rsa_is_probable_prime(&c97, 20));
    slang_slice_release(&m127);
    slang_slice_release(&f7);
    slang_slice_release(&c561);
    slang_slice_release(&c97);
}

/* Brotli: a dictionary-heavy vector, then the brotli CLI (when installed)
 * on text and binary inputs at several qualities and window sizes. */
TEST(brotli_decoder) {
    slang_slice in = unhex("1f5c00e08d54b5bf36437ba35320652d8c219d681336e088b3f0e151177e208715946d07b71f6d"), out;
    slang_error *err = NULL;
    slang_compress_rt_brotli_decode(&in, &out, &err);
    const char *want = "The government of the United States, information about international business and the people.";
    assert(!err && out.len == (int64_t)strlen(want) && memcmp(out.data, want, (size_t)out.len) == 0);
    slang_slice_release(&out);
    in.len = 10;   /* truncated */
    slang_compress_rt_brotli_decode(&in, &out, &err);
    assert(err);
    slang_release(err);
    err = NULL;
    slang_slice_release(&in);
    if (system("brotli --version >/dev/null 2>&1") != 0) {
        printf("  (brotli CLI not found: fixed vectors only)\n");
        return;
    }
    size_t n = 300000;
    uint8_t *buf = malloc(n);
    static const char *words[] = { "the ", "government ", "Information ", "of ", "and ", "people ", "\n", "THE " };
    for (int kind = 0; kind < 2; kind++) {
        size_t i = 0;
        while (i < n) {
            if (kind == 0) {
                const char *w = words[xr() % 8];
                for (size_t j = 0; w[j] && i < n; j++) buf[i++] = (uint8_t)w[j];
            } else {
                buf[i++] = (uint8_t)(xr() % 5 == 0 ? xr() : 'z');
            }
        }
        char src[] = "/tmp/slang_br_XXXXXX";
        int fd = mkstemp(src);
        assert(fd >= 0 && write(fd, buf, n) == (ssize_t)n);
        close(fd);
        static const char *opts[] = { "-q 0", "-q 5", "-q 11", "-q 9 -w 10", "-q 9 -w 24" };
        for (int k = 0; k < 5; k++) {
            char cmd[256], dst[64];
            snprintf(dst, sizeof dst, "%s.br", src);
            snprintf(cmd, sizeof cmd, "brotli %s -f -c %s > %s", opts[k], src, dst);
            assert(system(cmd) == 0);
            FILE *f = fopen(dst, "rb");
            assert(f);
            fseek(f, 0, SEEK_END);
            long len = ftell(f);
            fseek(f, 0, SEEK_SET);
            uint8_t *c = malloc((size_t)len);
            assert(fread(c, 1, (size_t)len, f) == (size_t)len);
            fclose(f);
            slang_slice cs = bytes(c, (size_t)len);
            slang_compress_rt_brotli_decode(&cs, &out, &err);
            if (err) fprintf(stderr, "brotli %s: %s\n", opts[k], slang_string_data(err->message));
            assert(!err && out.len == (int64_t)n && memcmp(out.data, buf, n) == 0);
            slang_slice_release(&out);
            slang_slice_release(&cs);
            free(c);
            unlink(dst);
        }
        unlink(src);
    }
    free(buf);
}

TEST(sql_sentinels) {
    slang_error *e = slang_database_sql_err_no_rows();
    assert(STR_IS(e->message, "sql: no rows in result set"));
    slang_release(e);   /* immortal */
    assert(slang_database_sql_err_no_rows() == e);
}

int main(void) {
    printf("test_std4\n");
    RUN(zstd_encoder);
    RUN(argon2_rfc9106);
    RUN(pbkdf2_scrypt_blake2b);
    RUN(p384_rfc6979);
    RUN(aes_cbc);
    RUN(rsa_primes);
    RUN(brotli_decoder);
    RUN(sql_sentinels);
    return 0;
}
