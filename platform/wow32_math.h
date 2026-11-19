#ifndef BEER_WOW32_MATH_H
#define BEER_WOW32_MATH_H
/* libm for the 32-bit (WoW32) C runtime. The loader proper is built without SSE, so it cannot pass or return doubles
 * by value; this unit is built with SSE and takes pointers. */
enum { W32_MATH_FLOOR, W32_MATH_CEIL, W32_MATH_SQRT, W32_MATH_FABS, W32_MATH_SIN, W32_MATH_COS, W32_MATH_TAN,
       W32_MATH_ATAN, W32_MATH_EXP, W32_MATH_LOG, W32_MATH_LOG10, W32_MATH_POW, W32_MATH_ATAN2, W32_MATH_FMOD,
       W32_MATH_LDEXP };
/* in[0], in[1] are the operands (for LDEXP in[1] is the integer exponent); the result is stored in *out. */
void beer_w32_math(int op, const double *in, double *out);
void beer_w32_strtod(const char *text, double *out, char **end);
#endif
