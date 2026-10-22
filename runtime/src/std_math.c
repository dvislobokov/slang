/* std_math.c - math (zell_math_*). IEEE-754 double semantics as in Go's
 * math package; Min/Max follow Go (NaN propagates, -0 < +0). */
#include "rt_internal.h"

#include <math.h>

double zell_math_sqrt(double x) { return sqrt(x); }
double zell_math_abs(double x) { return fabs(x); }
double zell_math_pow(double x, double y) { return pow(x, y); }
double zell_math_floor(double x) { return floor(x); }
double zell_math_ceil(double x) { return ceil(x); }
double zell_math_sin(double x) { return sin(x); }
double zell_math_cos(double x) { return cos(x); }
double zell_math_tan(double x) { return tan(x); }
double zell_math_log(double x) { return log(x); }
double zell_math_exp(double x) { return exp(x); }

double zell_math_min(double x, double y) {
    if (isinf(x) && x < 0) return x;
    if (isinf(y) && y < 0) return y;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0 && y == 0) return signbit(x) ? x : y;
    return x < y ? x : y;
}

double zell_math_max(double x, double y) {
    if (isinf(x) && x > 0) return x;
    if (isinf(y) && y > 0) return y;
    if (isnan(x) || isnan(y)) return NAN;
    if (x == 0 && y == 0) return signbit(x) ? y : x;
    return x > y ? x : y;
}
