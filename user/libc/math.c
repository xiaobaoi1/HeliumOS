#include "math.h"

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
    long r = (long)x;
    if (x < 0 && (double)r != x) r--;
    return (double)r;
}

double ceil(double x) {
    long r = (long)x;
    if (x > 0 && (double)r != x) r++;
    return (double)r;
}

double round(double x) { return x >= 0 ? floor(x + 0.5) : ceil(x - 0.5); }

double sqrt(double x) {
    if (x <= 0) return 0;
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 30; i++) r = 0.5 * (r + x / r);
    return r;
}

static double sin_taylor(double x) {
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n < 20; n++) {
        term *= -x2 / ((2.0 * n) * (2.0 * n + 1));
        sum += term;
    }
    return sum;
}

double sin(double x) {
    const double two_pi = 6.283185307179586;
    long k = (long)(x / two_pi);
    x -= (double)k * two_pi;
    if (x > M_PI)  x -= two_pi;
    if (x < -M_PI) x += two_pi;
    return sin_taylor(x);
}

double cos(double x) { return sin(x + M_PI_2); }

double tan(double x) {
    double c = cos(x);
    return fabs(c) < 1e-12 ? 0 : sin(x) / c;
}

double atan(double x) {
    int neg = x < 0;
    if (neg) x = -x;
    int inv = x > 1;
    if (inv) x = 1 / x;
    double x2 = x * x, term = x, sum = x;
    for (int n = 3; n < 50; n += 2) {
        term *= -x2;
        sum += term / n;
    }
    if (inv) sum = M_PI_2 - sum;
    return neg ? -sum : sum;
}

double atan2(double y, double x) {
    if (x > 0) return atan(y / x);
    if (x < 0) return atan(y / x) + (y >= 0 ? M_PI : -M_PI);
    if (y > 0) return M_PI_2;
    if (y < 0) return -M_PI_2;
    return 0;
}

double asin(double x) { return atan2(x, sqrt(1 - x * x)); }
double acos(double x) { return atan2(sqrt(1 - x * x), x); }

double exp(double x) {
    const double ln2 = 0.6931471805599453;
    long k = (long)(x / ln2);
    double r = x - (double)k * ln2;
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 30; n++) { term *= r / n; sum += term; }
    if (k > 0) while (k--) sum *= 2;
    else       while (k++) sum *= 0.5;
    return sum;
}

double log(double x) {
    if (x <= 0) return 0;
    long k = 0;
    while (x >= 2) { x *= 0.5; k++; }
    while (x < 1)  { x *= 2;   k--; }
    double z = (x - 1) / (x + 1);
    double z2 = z * z, term = z, sum = z;
    for (int n = 3; n < 60; n += 2) { term *= z2; sum += term / n; }
    return 2 * sum + (double)k * 0.6931471805599453;
}

double log10(double x) { return log(x) / 2.302585092994046; }

double pow(double x, double y) {
    if (x == 0) return y > 0 ? 0 : 1;
    int iy = (int)y;
    if ((double)iy == y) {
        int neg = iy < 0;
        if (neg) iy = -iy;
        double r = 1;
        for (int i = 0; i < iy; i++) r *= x;
        return neg ? 1 / r : r;
    }
    return exp(y * log(fabs(x)));
}