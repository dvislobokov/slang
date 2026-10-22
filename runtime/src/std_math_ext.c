/* std_math_ext.c - more std.math (zell_math_*). Float functions are C libm
 * with Go's math semantics; IMin/IMax/IAbs are the int counterparts of the
 * float Min/Max/Abs (Zell has no overloading). */
#include "rt_internal.h"

#include <math.h>

double zell_math_round(double x) { return round(x); }   /* half away from zero */
double zell_math_trunc(double x) { return trunc(x); }
double zell_math_mod(double x, double y) { return fmod(x, y); }
double zell_math_hypot(double x, double y) { return hypot(x, y); }
double zell_math_atan2(double y, double x) { return atan2(y, x); }
double zell_math_log2(double x) { return log2(x); }
double zell_math_log10(double x) { return log10(x); }
bool zell_math_is_nan(double x) { return isnan(x); }

bool zell_math_is_inf(double x, int64_t sign) {
    return (sign >= 0 && x == INFINITY) || (sign <= 0 && x == -INFINITY);
}

double zell_math_inf(int64_t sign) { return sign >= 0 ? INFINITY : -INFINITY; }
double zell_math_nan(void) { return NAN; }

int64_t zell_math_imin(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t zell_math_imax(int64_t a, int64_t b) { return a > b ? a : b; }
/* IAbs(MinInt) wraps to MinInt (two's complement), no panic. */
int64_t zell_math_iabs(int64_t a) { return a < 0 ? (int64_t)(0 - (uint64_t)a) : a; }
