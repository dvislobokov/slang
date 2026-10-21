/* macOS stub SDK (runtime/stub-sdk/macos/README.md): compile checks only. */
#ifndef SLANG_STUB_MATH_H
#define SLANG_STUB_MATH_H
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define M_PI 3.14159265358979323846264338327950288
#define M_E 2.71828182845904523536028747135266250
#define M_LN2 0.693147180559945309417232121458176568
#define M_LN10 2.30258509299404568401799145468436421
#define M_SQRT2 1.41421356237309504880168872420969808
#define M_LOG2E 1.44269504088896340735992468100189214
#define M_LOG10E 0.434294481903251827651128918916605082
#define M_PI_2 1.57079632679489661923132169163975144
#define M_PI_4 0.785398163397448309615660845819875721
#define M_1_PI 0.318309886183790671537767526745028724
#define M_2_PI 0.636619772367581343075535053490057448
#define M_2_SQRTPI 1.12837916709551257389615890312154517
#define M_SQRT1_2 0.707106781186547524400844362104849039
#define FP_NAN 1
#define FP_INFINITE 2
#define FP_ZERO 3
#define FP_NORMAL 4
#define FP_SUBNORMAL 5
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isgreater(x, y) __builtin_isgreater(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define isunordered(x, y) __builtin_isunordered(x, y)
typedef float float_t;
typedef double double_t;
#define SLANG_STUB_MATH1(n) double n(double); float n##f(float); long double n##l(long double);
#define SLANG_STUB_MATH2(n) double n(double, double); float n##f(float, float); long double n##l(long double, long double);
SLANG_STUB_MATH1(sin) SLANG_STUB_MATH1(cos) SLANG_STUB_MATH1(tan) SLANG_STUB_MATH1(asin) SLANG_STUB_MATH1(acos) SLANG_STUB_MATH1(atan)
SLANG_STUB_MATH1(sinh) SLANG_STUB_MATH1(cosh) SLANG_STUB_MATH1(tanh) SLANG_STUB_MATH1(asinh) SLANG_STUB_MATH1(acosh) SLANG_STUB_MATH1(atanh)
SLANG_STUB_MATH1(exp) SLANG_STUB_MATH1(exp2) SLANG_STUB_MATH1(expm1) SLANG_STUB_MATH1(log) SLANG_STUB_MATH1(log2) SLANG_STUB_MATH1(log10) SLANG_STUB_MATH1(log1p)
SLANG_STUB_MATH1(sqrt) SLANG_STUB_MATH1(cbrt) SLANG_STUB_MATH1(fabs) SLANG_STUB_MATH1(floor) SLANG_STUB_MATH1(ceil) SLANG_STUB_MATH1(round) SLANG_STUB_MATH1(trunc)
SLANG_STUB_MATH1(rint) SLANG_STUB_MATH1(nearbyint) SLANG_STUB_MATH1(erf) SLANG_STUB_MATH1(erfc) SLANG_STUB_MATH1(tgamma) SLANG_STUB_MATH1(lgamma) SLANG_STUB_MATH1(logb)
SLANG_STUB_MATH2(atan2) SLANG_STUB_MATH2(pow) SLANG_STUB_MATH2(fmod) SLANG_STUB_MATH2(remainder) SLANG_STUB_MATH2(hypot) SLANG_STUB_MATH2(copysign)
SLANG_STUB_MATH2(fmax) SLANG_STUB_MATH2(fmin) SLANG_STUB_MATH2(fdim) SLANG_STUB_MATH2(nextafter)
double fma(double, double, double);
float fmaf(float, float, float);
double frexp(double, int *);
double ldexp(double, int);
float ldexpf(float, int);
double scalbn(double, int);
double modf(double, double *);
float modff(float, float *);
long lround(double);
long long llround(double);
long lrint(double);
long long llrint(double);
int ilogb(double);
double j0(double);
double j1(double);
double jn(int, double);
double y0(double);
double y1(double);
double yn(int, double);
double lgamma_r(double, int *);
#undef SLANG_STUB_MATH1
#undef SLANG_STUB_MATH2
#endif
