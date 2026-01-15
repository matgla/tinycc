/*
 * Soft-float Comparison - Double Precision
 * Implements ARM EABI comparison functions for double-precision floats
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Core comparison returning -1 (a<b), 0 (a==b), 1 (a>b), 2 (unordered/NaN) */
static int dcmp_core(double a, double b)
{
  union
  {
    double d;
    uint64_t u;
  } ua, ub;
  ua.d = a;
  ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;

  /* Check for NaN */
  if (is_nan_bits(a_bits) || is_nan_bits(b_bits))
    return 2;

  /* Handle zeros (+0 == -0) */
  if (is_zero_bits(a_bits) && is_zero_bits(b_bits))
    return 0;

  int a_sign = double_sign(a_bits);
  int b_sign = double_sign(b_bits);

  /* Different signs: negative < positive */
  if (a_sign != b_sign)
  {
    return a_sign ? -1 : 1; /* if a is negative, a < b */
  }

  /* Same sign: compare magnitude */
  /* For positive numbers, larger bits = larger value */
  /* For negative numbers, larger bits = smaller value */
  uint64_t a_mag = a_bits & ~DOUBLE_SIGN_BIT;
  uint64_t b_mag = b_bits & ~DOUBLE_SIGN_BIT;

  if (a_mag == b_mag)
    return 0;

  int mag_cmp = (a_mag > b_mag) ? 1 : -1;

  /* If negative, invert the comparison */
  return a_sign ? -mag_cmp : mag_cmp;
}

/* Compare for equal: __aeabi_dcmpeq
 * Returns 1 if a == b, 0 otherwise
 */
int __aeabi_dcmpeq(double a, double b)
{
  return dcmp_core(a, b) == 0 ? 1 : 0;
}

/* Compare for less than: __aeabi_dcmplt
 * Returns 1 if a < b, 0 otherwise
 */
int __aeabi_dcmplt(double a, double b)
{
  return dcmp_core(a, b) == -1 ? 1 : 0;
}

/* Compare for less than or equal: __aeabi_dcmple
 * Returns 1 if a <= b, 0 otherwise
 */
int __aeabi_dcmple(double a, double b)
{
  int r = dcmp_core(a, b);
  return (r == -1 || r == 0) ? 1 : 0;
}

/* Compare for greater than: __aeabi_dcmpgt
 * Returns 1 if a > b, 0 otherwise
 */
int __aeabi_dcmpgt(double a, double b)
{
  return dcmp_core(a, b) == 1 ? 1 : 0;
}

/* Compare for greater than or equal: __aeabi_dcmpge
 * Returns 1 if a >= b, 0 otherwise
 */
int __aeabi_dcmpge(double a, double b)
{
  int r = dcmp_core(a, b);
  return (r == 1 || r == 0) ? 1 : 0;
}

/* Compare unordered: __aeabi_dcmpun
 * Returns 1 if either a or b is NaN, 0 otherwise
 */
int __aeabi_dcmpun(double a, double b)
{
  return dcmp_core(a, b) == 2 ? 1 : 0;
}

/* Wrapper functions with 'c' prefix that set ARM CPSR flags */

int __aeabi_cdcmple(double a, double b)
{
  return __aeabi_dcmple(a, b);
}

int __aeabi_cdrcmple(double a, double b)
{
  return __aeabi_dcmple(b, a);
}

int __aeabi_cdcmplt(double a, double b)
{
  return __aeabi_dcmplt(a, b);
}

int __aeabi_cdcmpeq(double a, double b)
{
  return __aeabi_dcmpeq(a, b);
}

int __aeabi_cdcmpgt(double a, double b)
{
  return __aeabi_dcmpgt(a, b);
}

int __aeabi_cdcmpge(double a, double b)
{
  return __aeabi_dcmpge(a, b);
}
