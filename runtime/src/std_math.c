/* std_math.c - std.math (slang_math_*). IEEE-754 double semantics as in Go's
 * math package; Min/Max follow Go (NaN propagates, -0 < +0). */
#include "rt_internal.h"

#include <math.h>

double slang_math_sqrt(double x) { return sqrt(x); }
double slang_math_abs(double x) { return fabs(x); }
double slang_math_pow(double x, double y) { return pow(x, y); }
double slang_math_floor(double x) { return floor(x); }
double slang_math_ceil(double x) { return ceil(x); }
double slang_math_sin(double x) { return sin(x); }
double slang_math_cos(double x) { return cos(x); }
double slang_math_tan(double x) { return tan(x); }
double slang_math_log(double x) { return log(x); }
double slang_math_exp(double x) { return exp(x); }

double slang_math_min(double x, double y) {
    if (isinf(x) && x < 0) return x;
    if (isinf(y) && y < 0) return y;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0 && y == 0) return signbit(x) ? x : y;
    return x < y ? x : y;
}

double slang_math_max(double x, double y) {
    if (isinf(x) && x > 0) return x;
    if (isinf(y) && y > 0) return y;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0 && y == 0) return signbit(x) ? y : x;
    return x > y ? x : y;
}
