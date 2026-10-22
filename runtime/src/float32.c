/* float32.c - float32 formatting support: Zell prints a float32 as the
 * shortest decimal that round-trips as a float32 (like Go). */
#include "rt_internal.h"

#include <stdio.h>
#include <stdlib.h>

double zell_float32_shortest(float f) {
    if (f != f || f - f != 0) return (double)f; /* NaN, Inf */
    char buf[48];
    for (int prec = 1; prec <= 9; prec++) {
        snprintf(buf, sizeof buf, "%.*g", prec, (double)f);
        if (strtof(buf, NULL) == f) return strtod(buf, NULL);
    }
    return (double)f;
}
