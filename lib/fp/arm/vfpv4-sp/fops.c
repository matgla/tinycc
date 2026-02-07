/*
 * ARM VFPv4 Single-Precision Hardware FP Operations
 * Optimized for Cortex-M4F with hardware single-precision FPU
 * Double-precision operations fall back to software
 */

#include "../../fp_abi.h"

/* Single-precision addition: VFPv4 hardware */
float __aeabi_fadd(float a, float b)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t" /* Load a into s0 */
                   "vmov    s1, %2        \n\t" /* Load b into s1 */
                   "vadd.f32 s0, s0, s1   \n\t" /* Add: s0 = s0 + s1 */
                   "vmov    %0, s0        \n\t" /* Store result */
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

/* Single-precision negation */
float __aeabi_fneg(float a)
{
  float result;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vneg.f32 s0, s0       \n\t"
                   "vmov    %0, s0        \n\t"
                   : "=r"(result)
                   : "r"(a));
  return result;
}
