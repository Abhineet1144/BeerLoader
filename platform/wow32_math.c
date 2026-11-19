#include <math.h>
#include <stdlib.h>
#include "wow32_math.h"

void beer_w32_math(int op, const double *in, double *out)
{
    const double a = in[0], b = in[1];
    switch (op) {
    case W32_MATH_FLOOR: *out = floor(a); break;
    case W32_MATH_CEIL:  *out = ceil(a); break;
    case W32_MATH_SQRT:  *out = sqrt(a); break;
    case W32_MATH_FABS:  *out = fabs(a); break;
    case W32_MATH_SIN:   *out = sin(a); break;
    case W32_MATH_COS:   *out = cos(a); break;
    case W32_MATH_TAN:   *out = tan(a); break;
    case W32_MATH_ATAN:  *out = atan(a); break;
    case W32_MATH_EXP:   *out = exp(a); break;
    case W32_MATH_LOG:   *out = log(a); break;
    case W32_MATH_LOG10: *out = log10(a); break;
    case W32_MATH_POW:   *out = pow(a, b); break;
    case W32_MATH_ATAN2: *out = atan2(a, b); break;
    case W32_MATH_FMOD:  *out = fmod(a, b); break;
    case W32_MATH_LDEXP: *out = ldexp(a, (int)b); break;
    default:             *out = 0; break;
    }
}

void beer_w32_strtod(const char *text, double *out, char **end) { *out = strtod(text, end); }
