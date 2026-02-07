/*
 * ARM VFPv5 Double-Precision Conversions
 * Float to/from integer conversions using hardware
 */

#include "../../fp_abi.h"

/* Convert double to signed integer */
int __aeabi_d2iz(double a)
{
  int32_t result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vcvt.s32.f64 s0, d0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a_lo), "r"(a_hi));
  return result;
}

/* Convert double to unsigned integer */
unsigned int __aeabi_d2uiz(double a)
{
  uint32_t result;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vcvt.u32.f64 s0, d0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a_lo), "r"(a_hi));
  return result;
}

/* Convert signed integer to double */
double __aeabi_i2d(int a)
{
  double result;
  uint32_t r0, r1;
  __asm__ volatile("vmov    s0, %2        \n\t" /* Move a to s0 */
                   "vcvt.f64.s32 d0, s0   \n\t" /* Convert s32 to f64 in d0 */
                   "vmov    %0, %1, d0    \n\t" /* Move d0 to r0 (low), r1 (high) */
                   : "=r"(r0), "=r"(r1)
                   : "r"(a));
  /* Cast the two 32-bit registers back to double */
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Convert unsigned integer to double */
double __aeabi_ui2d(unsigned int a)
{
  double result;
  uint32_t r0, r1;
  __asm__ volatile("vmov    s0, %2        \n\t" /* Move a to s0 */
                   "vcvt.f64.u32 d0, s0   \n\t" /* Convert u32 to f64 in d0 */
                   "vmov    %0, %1, d0    \n\t" /* Move d0 to r0 (low), r1 (high) */
                   : "=r"(r0), "=r"(r1)
                   : "r"(a));
  /* Cast the two 32-bit registers back to double */
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Convert float to double */
double __aeabi_f2d(float a)
{
  double result;
  uint32_t r0, r1;
  __asm__ volatile("vmov    s0, %2        \n\t" /* Move a to s0 */
                   "vcvt.f64.f32 d0, s0   \n\t" /* Convert f32 to f64 in d0 */
                   "vmov    %0, %1, d0    \n\t" /* Move d0 to r0 (low), r1 (high) */
                   : "=r"(r0), "=r"(r1)
                   : "r"(a));
  /* Cast the two 32-bit registers back to double */
  result = *(const double *)&(union {
              uint32_t u[2];
              double d;
            }){
      .u = {r0,
            r1}}.d;
  return result;
}

/* Convert double to float */
float __aeabi_d2f(double a)
{
  float result;
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vcvt.f32.f64 s0, d0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a), "r"(a));
  return result;
}

/* Convert float to signed integer */
int __aeabi_f2iz(float a)
{
  int32_t result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vcvt.s32.f32 s0, s0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a));
  return result;
}

/* Convert float to unsigned integer */
unsigned int __aeabi_f2uiz(float a)
{
  uint32_t result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vcvt.u32.f32 s0, s0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a));
  return result;
}

/* Convert signed integer to float */
float __aeabi_i2f(int a)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vcvt.f32.s32 s0, s0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a));
  return result;
}

/* Convert unsigned integer to float */
float __aeabi_ui2f(unsigned int a)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vcvt.f32.u32 s0, s0   \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a));
  return result;
}
