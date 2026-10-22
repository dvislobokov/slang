/* std_random.c - random (zell_random_*): a non-cryptographic PRNG,
 * xoshiro256** (Blackman & Vigna) seeded through splitmix64. One state per
 * process, guarded by a mutex; seeded from getrandom(2) on first use unless
 * Seed() is called. The sequence after Seed(n) is fixed for a given n
 * (documented in ABI.md section 14 so programs/tests can rely on it). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <string.h>
#include <time.h>

static zell__plat_mutex g_lock = ZELL_PLAT_MUTEX_INIT;
static uint64_t g_s[4];
static bool g_seeded;

static uint64_t splitmix64(uint64_t *x) {
    uint64_t z = (*x += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void seed_locked(uint64_t seed) {
    uint64_t x = seed;
    for (int i = 0; i < 4; i++) g_s[i] = splitmix64(&x);
    g_seeded = true;
}

static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t next_locked(void) {
    if (!g_seeded) {
        uint64_t seed = 0;
        if (!zell__plat_random_bytes(&seed, sizeof seed)) {
            int64_t ns = zell__plat_wall_ns();
            seed = (uint64_t)(ns / 1000000000) * 1000000007ULL ^ (uint64_t)(ns % 1000000000) ^ ((uint64_t)zell__plat_pid() << 32);
        }
        seed_locked(seed);
    }
    uint64_t r = rotl(g_s[1] * 5, 7) * 9, t = g_s[1] << 17;
    g_s[2] ^= g_s[0];
    g_s[3] ^= g_s[1];
    g_s[1] ^= g_s[2];
    g_s[0] ^= g_s[3];
    g_s[2] ^= t;
    g_s[3] = rotl(g_s[3], 45);
    return r;
}

static uint64_t next(void) {
    zell__plat_mutex_lock(&g_lock);
    uint64_t r = next_locked();
    zell__plat_mutex_unlock(&g_lock);
    return r;
}

/* Unbiased value in [0, n) for n > 0 (rejection sampling). */
static uint64_t below_locked(uint64_t n) {
    uint64_t limit = UINT64_MAX - UINT64_MAX % n;   /* multiple of n */
    uint64_t r;
    do r = next_locked(); while (r >= limit);
    return r % n;
}

void zell_random_seed(int64_t seed) {
    zell__plat_mutex_lock(&g_lock);
    seed_locked((uint64_t)seed);
    zell__plat_mutex_unlock(&g_lock);
}

int64_t zell_random_int(void) { return (int64_t)(next() >> 1); }

int64_t zell_random_int_n(int64_t n) {
    if (n <= 0) zell_panic("random: invalid argument to IntN");
    zell__plat_mutex_lock(&g_lock);
    uint64_t r = below_locked((uint64_t)n);
    zell__plat_mutex_unlock(&g_lock);
    return (int64_t)r;
}

double zell_random_float(void) { return (double)(next() >> 11) * 0x1.0p-53; }

void zell_random_bytes(int64_t n, zell_slice *out) {
    if (n < 0) zell_panic("random: negative length in Bytes");
    zell_slice_new(out, 1, n, n);
    uint8_t *p = out->data;
    zell__plat_mutex_lock(&g_lock);
    for (int64_t i = 0; i < n; i += 8) {
        uint64_t r = next_locked();
        int64_t k = n - i < 8 ? n - i : 8;
        memcpy(p + i, &r, (size_t)k);   /* little-endian byte order of r */
    }
    zell__plat_mutex_unlock(&g_lock);
}

/* In-place Fisher-Yates shuffle of an []int (slices share their buffer). */
void zell_random_shuffle(const zell_slice *s) {
    int64_t n = s ? s->len : 0;
    if (n < 2) return;
    int64_t *a = s->data;
    zell__plat_mutex_lock(&g_lock);
    for (int64_t i = n - 1; i > 0; i--) {
        int64_t j = (int64_t)below_locked((uint64_t)i + 1);
        int64_t t = a[i]; a[i] = a[j]; a[j] = t;
    }
    zell__plat_mutex_unlock(&g_lock);
}
