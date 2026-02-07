/*
 * ARM VFPv4 Single-Precision Comparison Operations
 * Cortex-M4F has hardware float comparison
 */

#include "../../fp_abi.h"

/* Compare single-precision floats and set APSR flags */
int __aeabi_cfcmple(float a, float b)
{
  uint32_t flags;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vcmp.f32 s0, s1       \n\t" /* Compare s0 with s1 */
                   "vmrs    %0, fpscr     \n\t" /* Move FP status to register */
                   : "=r"(flags)
                   : "r"(a), "r"(b));
  return flags;
}

/* Compare with reversed operands */
int __aeabi_cfrcmple(float a, float b)
{
  return __aeabi_cfcmple(b, a);
}

/* Less than comparison */
int __aeabi_cfcmplt(float a, float b)
{
  uint32_t flags;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vcmp.f32 s0, s1       \n\t"
                   "vmrs    %0, fpscr     \n\t" /* Move FP status to register */
                   : "=r"(flags)
                   : "r"(a), "r"(b));
  return flags;
}

/* Greater than or equal */
int __aeabi_cfcmpge(float a, float b)
{
  return __aeabi_cfcmple(b, a);
}

/* Greater than */
int __aeabi_cfcmpgt(float a, float b)
{
  return __aeabi_cfcmplt(b, a);
}

/* Equal comparison */
int __aeabi_cfcmpeq(float a, float b)
{
  uint32_t flags;
  __asm__ volatile("vmov    s0, %1        \n\t"
                   "vmov    s1, %2        \n\t"
                   "vcmp.f32 s0, s1       \n\t"
                   "vmrs    %0, fpscr     \n\t" /* Move FP status to register */
                   : "=r"(flags)
                   : "r"(a), "r"(b));
  return flags;
}
