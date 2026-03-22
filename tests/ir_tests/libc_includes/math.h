#pragma once

double sin(double arg);
double cos(double arg);
double tan(double arg);
double asin(double arg);
double acos(double arg);
double atan(double arg);
double atan2(double y, double x);
double sinh(double arg);
double cosh(double arg);
double tanh(double arg);
double exp(double arg);
double log(double arg);
double log10(double arg);
double pow(double base, double exponent);
double sqrt(double arg);
double cbrt(double arg);
double ceil(double arg);
double floor(double arg);
double round(double arg);
double trunc(double arg);
double fabs(double arg);
double fmod(double x, double y);
double remainder(double x, double y);

float sinf(float arg);
float cosf(float arg);
float tanf(float arg);
float asinf(float arg);
float acosf(float arg);
float atanf(float arg);
float atan2f(float y, float x);
float sinhf(float arg);
float coshf(float arg);
float tanhf(float arg);
float expf(float arg);
float logf(float arg);
float log10f(float arg);
float powf(float base, float exponent);
float sqrtf(float arg);
float cbrtf(float arg);
float ceilf(float arg);
float floorf(float arg);
float roundf(float arg);
float truncf(float arg);
float fabsf(float arg);
float fmodf(float x, float y);
float remainderf(float x, float y);

#define HUGE_VAL (__builtin_huge_val())
#define INFINITY (__builtin_inff())
#define NAN (__builtin_nanf(""))