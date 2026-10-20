/* std_math_bits.c - std.math.bits (Go's math/bits) on uint64 / uint32
 * with compiler builtins; the tuple-returning Add64/Sub64/Mul64/Div64 are
 * Slang wrappers (stdlib_src/bits.sl) over the rt* helpers here. */
#include "rt_internal.h"

#include <string.h>

int64_t slang_math_bits_leading_zeros(uint64_t x);
int64_t slang_math_bits_leading_zeros32(uint32_t x);
int64_t slang_math_bits_trailing_zeros(uint64_t x);
int64_t slang_math_bits_trailing_zeros32(uint32_t x);
int64_t slang_math_bits_ones_count(uint64_t x);
int64_t slang_math_bits_ones_count32(uint32_t x);
int64_t slang_math_bits_len(uint64_t x);
int64_t slang_math_bits_len32(uint32_t x);
uint64_t slang_math_bits_rotate_left(uint64_t x, int64_t k);
uint32_t slang_math_bits_rotate_left32(uint32_t x, int64_t k);
uint64_t slang_math_bits_reverse(uint64_t x);
uint32_t slang_math_bits_reverse32(uint32_t x);
uint64_t slang_math_bits_reverse_bytes(uint64_t x);
uint32_t slang_math_bits_reverse_bytes32(uint32_t x);
uint16_t slang_math_bits_reverse_bytes16(uint16_t x);
uint64_t slang_math_bits_rt_mul_hi(uint64_t x, uint64_t y);
uint64_t slang_math_bits_rt_div_quo(uint64_t hi, uint64_t lo, uint64_t y);
uint64_t slang_math_bits_rt_div_rem(uint64_t hi, uint64_t lo, uint64_t y);

int64_t slang_math_bits_leading_zeros(uint64_t x) { return x ? __builtin_clzll(x) : 64; }
int64_t slang_math_bits_leading_zeros32(uint32_t x) { return x ? __builtin_clz(x) : 32; }
int64_t slang_math_bits_trailing_zeros(uint64_t x) { return x ? __builtin_ctzll(x) : 64; }
int64_t slang_math_bits_trailing_zeros32(uint32_t x) { return x ? __builtin_ctz(x) : 32; }
int64_t slang_math_bits_ones_count(uint64_t x) { return __builtin_popcountll(x); }
int64_t slang_math_bits_ones_count32(uint32_t x) { return __builtin_popcount(x); }
int64_t slang_math_bits_len(uint64_t x) { return 64 - slang_math_bits_leading_zeros(x); }
int64_t slang_math_bits_len32(uint32_t x) { return 32 - slang_math_bits_leading_zeros32(x); }

/* Go: RotateLeft(x, k) rotates left by k mod 64; negative k rotates right. */
uint64_t slang_math_bits_rotate_left(uint64_t x, int64_t k) {
    unsigned s = (unsigned)(k & 63);
    return s ? (x << s) | (x >> (64 - s)) : x;
}
uint32_t slang_math_bits_rotate_left32(uint32_t x, int64_t k) {
    unsigned s = (unsigned)(k & 31);
    return s ? (x << s) | (x >> (32 - s)) : x;
}

uint64_t slang_math_bits_reverse(uint64_t x) {
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return __builtin_bswap64(x);
}
uint32_t slang_math_bits_reverse32(uint32_t x) { return (uint32_t)(slang_math_bits_reverse(x) >> 32); }
uint64_t slang_math_bits_reverse_bytes(uint64_t x) { return __builtin_bswap64(x); }
uint32_t slang_math_bits_reverse_bytes32(uint32_t x) { return __builtin_bswap32(x); }
uint16_t slang_math_bits_reverse_bytes16(uint16_t x) { return __builtin_bswap16(x); }

uint64_t slang_math_bits_rt_mul_hi(uint64_t x, uint64_t y) {
    return (uint64_t)(((unsigned __int128)x * y) >> 64);
}

/* Div64: panics like Go on y == 0 ("integer divide by zero") or overflow
 * (y <= hi). */
static void check_div(uint64_t hi, uint64_t y) {
    if (y == 0) slang_panic("integer divide by zero");
    if (y <= hi) slang_panic("integer overflow");
}
uint64_t slang_math_bits_rt_div_quo(uint64_t hi, uint64_t lo, uint64_t y) {
    check_div(hi, y);
    return (uint64_t)((((unsigned __int128)hi << 64) | lo) / y);
}
uint64_t slang_math_bits_rt_div_rem(uint64_t hi, uint64_t lo, uint64_t y) {
    check_div(hi, y);
    return (uint64_t)((((unsigned __int128)hi << 64) | lo) % y);
}

/* std.math.Float64bits & co (IEEE 754 bit patterns, Go's math package). */
uint64_t slang_math_float64bits(double f);
double slang_math_float64frombits(uint64_t b);
uint32_t slang_math_float32bits(float f);
float slang_math_float32frombits(uint32_t b);

uint64_t slang_math_float64bits(double f) {
    uint64_t b;
    memcpy(&b, &f, 8);
    return b;
}
double slang_math_float64frombits(uint64_t b) {
    double f;
    memcpy(&f, &b, 8);
    return f;
}
uint32_t slang_math_float32bits(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}
float slang_math_float32frombits(uint32_t b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
