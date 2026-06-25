/*
 * ARM VFPv4-sp Stub for Double-Precision Operations
 * VFPv4-sp only supports single-precision hardware FP
 * Double operations fall back to software implementation
 * This file provides stubs that link to soft float library
 */

#include "../../fp_abi.h"

/* External soft-float double functions */
extern double __aeabi_dadd(double a, double b);
extern double __aeabi_dsub(double a, double b);
extern double __aeabi_dmul(double a, double b);
extern double __aeabi_ddiv(double a, double b);
extern int __aeabi_cdcmple(double a, double b);
extern int __aeabi_d2iz(double a);
extern unsigned int __aeabi_d2uiz(double a);
extern double __aeabi_i2d(int a);
extern double __aeabi_ui2d(unsigned int a);
extern float __aeabi_d2f(double a);
extern double __aeabi_f2d_bits(uint32_t bits);

double __aeabi_f2d(float a)
{
  union {
    float f;
    uint32_t u;
  } conv = {.f = a};
  return __aeabi_f2d_bits(conv.u);
}

/* Double-precision addition - delegated to soft float */
double __aeabi_dadd_wrapper(double a, double b)
{
  return __aeabi_dadd(a, b);
}

/* Double-precision subtraction - delegated to soft float */
double __aeabi_dsub_wrapper(double a, double b)
{
  return __aeabi_dsub(a, b);
}

/* Double-precision multiplication - delegated to soft float */
double __aeabi_dmul_wrapper(double a, double b)
{
  return __aeabi_dmul(a, b);
}

/* Double-precision division - delegated to soft float */
double __aeabi_ddiv_wrapper(double a, double b)
{
  return __aeabi_ddiv(a, b);
}
