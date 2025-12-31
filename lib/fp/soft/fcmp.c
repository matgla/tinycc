/*
 * Soft-float Comparison - Single Precision
 * Implements ARM EABI comparison functions for single-precision floats
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Core comparison returning -1 (a<b), 0 (a==b), 1 (a>b), 2 (unordered/NaN) */
static int fcmp_core(float a, float b)
{
  union
  {
    float f;
    uint32_t u;
  } ua = {.f = a}, ub = {.f = b};
  uint32_t a_bits = ua.u, b_bits = ub.u;

  /* Check for NaN */
  if (is_nan_f(a_bits) || is_nan_f(b_bits))
    return 2;

  /* Handle zeros (+0 == -0) */
  if (is_zero_f(a_bits) && is_zero_f(b_bits))
    return 0;

  int a_sign = float_sign(a_bits);
  int b_sign = float_sign(b_bits);

  /* Different signs: negative < positive */
  if (a_sign != b_sign)
  {
    return a_sign ? -1 : 1;
  }

  /* Same sign: compare magnitude */
  uint32_t a_mag = a_bits & ~FLOAT_SIGN_BIT;
  uint32_t b_mag = b_bits & ~FLOAT_SIGN_BIT;

  if (a_mag == b_mag)
    return 0;

  int mag_cmp = (a_mag > b_mag) ? 1 : -1;

  /* If negative, invert the comparison */
  return a_sign ? -mag_cmp : mag_cmp;
}

/* Compare for equal: __aeabi_fcmpeq */
int __aeabi_fcmpeq(float a, float b)
{
  return fcmp_core(a, b) == 0 ? 1 : 0;
}

/* Compare for less than: __aeabi_fcmplt */
int __aeabi_fcmplt(float a, float b)
{
  return fcmp_core(a, b) == -1 ? 1 : 0;
}

/* Compare for less than or equal: __aeabi_fcmple */
int __aeabi_fcmple(float a, float b)
{
  int r = fcmp_core(a, b);
  return (r == -1 || r == 0) ? 1 : 0;
}

/* Compare for greater than: __aeabi_fcmpgt */
int __aeabi_fcmpgt(float a, float b)
{
  return fcmp_core(a, b) == 1 ? 1 : 0;
}

/* Compare for greater than or equal: __aeabi_fcmpge */
int __aeabi_fcmpge(float a, float b)
{
  int r = fcmp_core(a, b);
  return (r == 1 || r == 0) ? 1 : 0;
}

/* Compare unordered: __aeabi_fcmpun */
int __aeabi_fcmpun(float a, float b)
{
  return fcmp_core(a, b) == 2 ? 1 : 0;
}

/* Wrapper functions with 'c' prefix that set ARM CPSR flags */

int __aeabi_cfcmple(float a, float b)
{
  return __aeabi_fcmple(a, b);
}

int __aeabi_cfrcmple(float a, float b)
{
  return __aeabi_fcmple(b, a);
}

int __aeabi_cfcmplt(float a, float b)
{
  return __aeabi_fcmplt(a, b);
}

int __aeabi_cfcmpge(float a, float b)
{
  return __aeabi_fcmpge(a, b);
}

int __aeabi_cfcmpgt(float a, float b)
{
  return __aeabi_fcmpgt(a, b);
}

int __aeabi_cfcmpeq(float a, float b)
{
  return __aeabi_fcmpeq(a, b);
}
