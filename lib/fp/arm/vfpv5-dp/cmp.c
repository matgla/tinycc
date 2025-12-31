/*
 * ARM VFPv5 Double-Precision Comparison Operations
 * Cortex-M7 has full hardware FP comparison
 */

#include "../../fp_abi.h"

/* Compare double-precision floats */
int __aeabi_cdcmple(double a, double b)
{
  uint32_t flags;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vmov    d1, %3, %4    \n\t"
                   "vcmp.f64 d0, d1       \n\t"
                   "vmrs    %0, fpscr     \n\t"
                   : "=r"(flags)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  return flags;
}

/* Compare with reversed operands */
int __aeabi_cdrcmple(double a, double b)
{
  return __aeabi_cdcmple(b, a);
}

/* Less than comparison */
int __aeabi_cdcmplt(double a, double b)
{
  uint32_t flags;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vmov    d1, %3, %4    \n\t"
                   "vcmp.f64 d0, d1       \n\t"
                   "vmrs    %0, fpscr     \n\t"
                   : "=r"(flags)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  return flags;
}

/* Greater than or equal */
int __aeabi_cdcmpge(double a, double b)
{
  return __aeabi_cdcmple(b, a);
}

/* Greater than */
int __aeabi_cdcmpgt(double a, double b)
{
  return __aeabi_cdcmplt(b, a);
}

/* Equal comparison */
int __aeabi_cdcmpeq(double a, double b)
{
  uint32_t flags;
  uint32_t a_lo = *(uint32_t *)&a;
  uint32_t a_hi = *((uint32_t *)&a + 1);
  uint32_t b_lo = *(uint32_t *)&b;
  uint32_t b_hi = *((uint32_t *)&b + 1);
  __asm__ volatile("vmov    d0, %1, %2    \n\t"
                   "vmov    d1, %3, %4    \n\t"
                   "vcmp.f64 d0, d1       \n\t"
                   "vmrs    %0, fpscr     \n\t"
                   : "=r"(flags)
                   : "r"(a_lo), "r"(a_hi), "r"(b_lo), "r"(b_hi));
  return flags;
}

/* Single-precision comparison */
int __aeabi_cfcmple(float a, float b)
{
  uint32_t flags;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vcmp.f32 s0, s1       \n\t"
                   "vmrs    %0, fpscr     \n\t"
                   : "=r"(flags)
                   : "r"(a), "r"(b));
  return flags;
}

/* Single-precision with reversed operands */
int __aeabi_cfrcmple(float a, float b)
{
  return __aeabi_cfcmple(b, a);
}

/* Single-precision less than */
int __aeabi_cfcmplt(float a, float b)
{
  uint32_t flags;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vcmp.f32 s0, s1       \n\t"
                   "vmrs    %0, fpscr     \n\t"
                   : "=r"(flags)
                   : "r"(a), "r"(b));
  return flags;
}
