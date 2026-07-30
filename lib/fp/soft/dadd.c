/*
 * Soft-float Addition - Double Precision
 * Implements __aeabi_dadd and __aeabi_dsub for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* Add two double-precision floats */
double __aeabi_dadd(double a, double b)
{
  union
  {
    double d;
    uint64_t u;
  } ua, ub, ur;
  ua.d = a;
  ub.d = b;
  uint64_t a_bits = ua.u, b_bits = ub.u;

  int a_sign = double_sign(a_bits);
  int b_sign = double_sign(b_bits);
  int a_exp = double_exp(a_bits);
  int b_exp = double_exp(b_bits);
  uint64_t a_mant = double_mant(a_bits);
  uint64_t b_mant = double_mant(b_bits);

  /* Handle NaN */
  if (is_nan_bits(a_bits))
  {
    ur.u = a_bits;
    return ur.d;
  }
  if (is_nan_bits(b_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }

  /* Handle infinity */
  if (is_inf_bits(a_bits))
  {
    if (is_inf_bits(b_bits) && (a_sign != b_sign))
    {
      /* inf + (-inf) = NaN */
      ur.u = 0x7FF8000000000000ULL;
      return ur.d;
    }
    ur.u = a_bits;
    return ur.d;
  }
  if (is_inf_bits(b_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }

  /* Handle zero.
   * IEEE 754 §6.3: when both operands are zero, the result is +0 unless
   * both are negative (round-to-nearest mode). */
  if (is_zero_bits(a_bits) && is_zero_bits(b_bits))
  {
    ur.u = (a_sign && b_sign) ? DOUBLE_SIGN_BIT : 0;
    return ur.d;
  }
  if (is_zero_bits(a_bits))
  {
    ur.u = b_bits;
    return ur.d;
  }
  if (is_zero_bits(b_bits))
  {
    ur.u = a_bits;
    return ur.d;
  }

  /* Add the implicit bit for normals.  A subnormal has a stored exponent of 0
   * but an *effective* exponent of 1 (it is 0.mant x 2^-1022, not 2^-1023);
   * using the stored 0 misaligns every subnormal by one binade. */
  if (a_exp != 0)
    a_mant |= DOUBLE_IMPLICIT_BIT;
  else
    a_exp = 1;
  if (b_exp != 0)
    b_mant |= DOUBLE_IMPLICIT_BIT;
  else
    b_exp = 1;

  /* Align exponents - shift smaller mantissa right.
   * Work with 3 extra low bits (guard/round/sticky) so bits shifted out
   * during alignment still participate in rounding.  Without them the
   * small operand vanished entirely: 1 + -2^53 returned -2^53 instead of
   * the exactly representable -(2^53-1) (gcc-torture ieee/pr28634). */
  int exp_diff = a_exp - b_exp;
  int result_exp;
  uint64_t result_mant;
  int result_sign;

  a_mant <<= 3;
  b_mant <<= 3;

  if (exp_diff > 0)
  {
    /* a has larger exponent */
    if (exp_diff < 64)
    {
      uint64_t lost = b_mant & ((1ULL << exp_diff) - 1);
      b_mant = (b_mant >> exp_diff) | (lost != 0);
    }
    else
      b_mant = (b_mant != 0);
    result_exp = a_exp;
  }
  else if (exp_diff < 0)
  {
    /* b has larger exponent */
    if (-exp_diff < 64)
    {
      uint64_t lost = a_mant & ((1ULL << -exp_diff) - 1);
      a_mant = (a_mant >> -exp_diff) | (lost != 0);
    }
    else
      a_mant = (a_mant != 0);
    result_exp = b_exp;
  }
  else
  {
    result_exp = a_exp;
  }

  /* Add or subtract mantissas based on signs */
  if (a_sign == b_sign)
  {
    /* Same sign: add mantissas */
    result_mant = a_mant + b_mant;
    result_sign = a_sign;
  }
  else
  {
    /* Different signs: subtract mantissas */
    if (a_mant >= b_mant)
    {
      result_mant = a_mant - b_mant;
      result_sign = a_sign;
    }
    else
    {
      result_mant = b_mant - a_mant;
      result_sign = b_sign;
    }

    /* Exact cancellation is +0 in round-to-nearest (IEEE 754 section 6.3). */
    if (result_mant == 0)
    {
      ur.u = 0;
      return ur.d;
    }
  }

  /* Normalization, gradual underflow to subnormals, round-to-nearest-even and
   * overflow to infinity all happen here.  The previous code normalized only
   * while result_exp > 0 and then flushed any result with result_exp <= 0 to
   * zero, so every subnormal result was lost. */
  ur.u = sfp_round_pack_double(result_sign, result_exp, result_mant);
  return ur.d;
}

/* Subtract two double-precision floats */
double __aeabi_dsub(double a, double b)
{
  /* Negate b and add */
  union
  {
    double d;
    uint64_t u;
  } ub;
  ub.d = b;
  ub.u ^= DOUBLE_SIGN_BIT; /* Flip sign bit */
  return __aeabi_dadd(a, ub.d);
}

double __aeabi_dneg(double a)
{
  union
  {
    double d;
    uint64_t u;
  } ua;
  ua.d = a;
  ua.u ^= DOUBLE_SIGN_BIT;
  return ua.d;
}
