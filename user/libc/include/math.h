#ifndef _MATH_H
#define _MATH_H

float math_add(float a, float b);
float math_sub(float a, float b);
float math_mul(float a, float b);
float math_div(float a, float b);
float math_abs(float x);
float math_floor(float x);
float math_ceil(float x);
float math_round(float x);
float math_sqrt(float x);
float math_sin(float x);
float math_cos(float x);
float math_tan(float x);
float math_exp(float x);
float math_ln(float x);
float math_log10(float x);
float math_pow(float x, float y);

float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float roundf(float x);
float sqrtf(float x);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float expf(float x);
float logf(float x);
float log10f(float x);
float powf(float x, float y);

#ifndef M_PI
#define M_PI   3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#ifndef M_PI_4
#define M_PI_4 0.785398163397448309616
#endif
#ifndef M_E
#define M_E    2.7182818284590452354
#endif

double fabs(double x);
double floor(double x);
double ceil(double x);
double round(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double log10(double x);
double pow(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);

/* TinyCC 常量解析使用；HeliumOS 的 long double 按 double 处理。 */
long double ldexpl(long double x, int exp);
long double frexpl(long double x, int *exp);

#endif
