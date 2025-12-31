/*
 * ARM VFPv5 Double-Precision Hardware FP Operations
 * Optimized for Cortex-M7 with full hardware double-precision FPU
 */

#include "../../fp_abi.h"

/* Double-precision addition */
double __aeabi_dadd(double a, double b)
{
  double result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  uint32_t r0, r1;
  __asm__ volatile("vmov    d0, %2, %3    \n\t" /* Load a into d0 */
                   "vmov    d1, %4, %5    \n\t" /* Load b into d1 */
                   "vadd.f64 d0, d0, d1   \n\t" /* Add: d0 = d0 + d1 */
                   "vmov    %0, %1, d0    \n\t" /* Store result */
                   : "=r"(r0), "=r"(r1)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Double-precision subtraction */
double __aeabi_dsub(double a, double b)
{
  double result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  uint32_t r0, r1;
  __asm__ volatile("vmov    d0, %2, %3    \n\t"
                   "vmov    d1, %4, %5    \n\t"
                   "vsub.f64 d0, d0, d1   \n\t"
                   "vmov    %0, %1, d0    \n\t"
                   : "=r"(r0), "=r"(r1)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Double-precision multiplication */
double __aeabi_dmul(double a, double b)
{
  double result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  uint32_t r0, r1;
  __asm__ volatile("vmov    d0, %2, %3    \n\t"
                   "vmov    d1, %4, %5    \n\t"
                   "vmul.f64 d0, d0, d1   \n\t"
                   "vmov    %0, %1, d0    \n\t"
                   : "=r"(r0), "=r"(r1)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Double-precision division */
double __aeabi_ddiv(double a, double b)
{
  double result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  uint32_t r0, r1;
  __asm__ volatile("vmov    d0, %2, %3    \n\t"
                   "vmov    d1, %4, %5    \n\t"
                   "vdiv.f64 d0, d0, d1   \n\t"
                   "vmov    %0, %1, d0    \n\t"
                   : "=r"(r0), "=r"(r1)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Double-precision negation */
double __aeabi_dneg(double a)
{
  double result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t r0, r1;
  __asm__ volatile("vmov    d0, %2, %3    \n\t"
                   "vneg.f64 d0, d0       \n\t"
                   "vmov    %0, %1, d0    \n\t"
                   : "=r"(r0), "=r"(r1)
                   : "r"(a_lo), "r"(a_hi));
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Single-precision addition (also available) */
float __aeabi_fadd(float a, float b)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vadd.f32 s0, s0, s1   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a), "r"(b));
  return result;
}

/* Single-precision subtraction */
float __aeabi_fsub(float a, float b)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vsub.f32 s0, s0, s1   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a), "r"(b));
  return result;
}

/* Single-precision multiplication */
float __aeabi_fmul(float a, float b)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vmul.f32 s0, s0, s1   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a), "r"(b));
  return result;
}

/* Single-precision division */
float __aeabi_fdiv(float a, float b)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vdiv.f32 s0, s0, s1   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a), "r"(b));
  return result;
}
