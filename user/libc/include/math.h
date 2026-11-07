#ifndef _MATH_H
#define _MATH_H

#define M_PI 3.14159265358979323846f

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

#endif