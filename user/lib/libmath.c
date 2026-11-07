#include <math.h>

#define PI_F 3.14159265358979323846f

int g_counter = 0;

int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }
int bump(void) { return ++g_counter; }

float math_add(float a, float b) { return a + b; }
float math_sub(float a, float b) { return a - b; }
float math_mul(float a, float b) { return a * b; }
float math_div(float a, float b) { return b == 0.0f ? 0.0f : a / b; }
float math_abs(float x) { return x < 0.0f ? -x : x; }
float math_floor(float x) {
    int i = (int)x;
    return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i;
}
float math_ceil(float x) {
    int i = (int)x;
    return (x > 0.0f && (float)i != x) ? (float)(i + 1) : (float)i;
}
float math_round(float x) { return x >= 0.0f ? math_floor(x + 0.5f) : math_ceil(x - 0.5f); }

float math_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 24; i++) r = 0.5f * (r + x / r);
    return r;
}

static float sin_taylor(float x) {
    float x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / (float)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}
static float cos_taylor(float x) {
    float x2 = x * x, term = 1.0f, sum = 1.0f;
    for (int n = 1; n < 12; n++) {
        term *= -x2 / (float)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}
static float reduce_pi(float x) {
    const float two_pi = 6.283185307179586f;
    int k = (int)(x / two_pi);
    x -= (float)k * two_pi;
    if (x > PI_F) x -= two_pi;
    if (x < -PI_F) x += two_pi;
    return x;
}
float math_sin(float x) { return sin_taylor(reduce_pi(x)); }
float math_cos(float x) { return cos_taylor(reduce_pi(x)); }
float math_tan(float x) {
    float c = math_cos(x);
    return math_abs(c) < 0.000001f ? 0.0f : math_sin(x) / c;
}

float math_exp(float x) {
    if (x > 80.0f) x = 80.0f;
    if (x < -80.0f) x = -80.0f;
    const float ln2 = 0.6931471805599453f;
    int k = (int)(x / ln2);
    float r = x - (float)k * ln2;
    float term = 1.0f, sum = 1.0f;
    for (int n = 1; n < 18; n++) { term *= r / (float)n; sum += term; }
    if (k > 0) while (k--) sum *= 2.0f;
    else while (k++) sum *= 0.5f;
    return sum;
}

float math_ln(float x) {
    if (x <= 0.0f) return 0.0f;
    int k = 0;
    while (x >= 1.5f) { x *= 0.5f; k++; }
    while (x < 0.75f) { x *= 2.0f; k--; }
    float z = (x - 1.0f) / (x + 1.0f);
    float z2 = z * z, term = z, sum = z;
    for (int n = 3; n < 40; n += 2) { term *= z2; sum += term / (float)n; }
    return 2.0f * sum + (float)k * 0.6931471805599453f;
}
float math_log10(float x) { return math_ln(x) / 2.302585092994046f; }

float math_pow(float x, float y) {
    if (x == 0.0f) return y > 0.0f ? 0.0f : 1.0f;
    if (x < 0.0f) {
        int iy = (int)y;
        if ((float)iy != y) return 0.0f;
        float p = math_exp(y * math_ln(-x));
        return (iy & 1) ? -p : p;
    }
    return math_exp(y * math_ln(x));
}

float fabsf(float x) { return math_abs(x); }
float floorf(float x) { return math_floor(x); }
float ceilf(float x) { return math_ceil(x); }
float roundf(float x) { return math_round(x); }
float sqrtf(float x) { return math_sqrt(x); }
float sinf(float x) { return math_sin(x); }
float cosf(float x) { return math_cos(x); }
float tanf(float x) { return math_tan(x); }
float expf(float x) { return math_exp(x); }
float logf(float x) { return math_ln(x); }
float log10f(float x) { return math_log10(x); }
float powf(float x, float y) { return math_pow(x, y); }