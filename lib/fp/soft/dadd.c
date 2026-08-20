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

  /* Classify from the fields already extracted above rather than through
   * is_nan_bits/is_inf_bits/is_zero_bits, which re-derive the exponent and
   * mantissa from the raw bits every time.  Those are pure and tcc does CSE
   * them within a block -- but each test here ends in a `return`, so every one
   * lands in its own block and the CSE cannot reach across.  The result was
   * ten exponent extracts and eleven mantissa masks in the generated function
   * against gcc's two and four, all of them on the common path: dadd has no
   * loop, so its cost is exactly the length of this one straight line.
   *
   * The tests are also NESTED rather than written as a flat sequence of
   * independent `if`s.  Flat, the special cases read
   *
   *     if (is_nan(a)) ...;  if (is_nan(b)) ...;
   *     if (a_max_exp) ...;  if (b_max_exp) ...;
   *     if (a_is_zero && b_is_zero) ...;  if (a_is_zero) ...;  if (b_is_zero) ...;
   *
   * which evaluates `a_exp == 0x7FF` twice, `b_exp == 0x7FF` three times, and
   * materializes two booleans that are each branched on twice -- 28 of the 37
   * instructions dadd spent before reaching any arithmetic.  Every one of
   * those re-tests is decided by an earlier branch on the same value, so a
   * compiler that threads jumps through duplicated test blocks (gcc does)
   * removes them; tcc does not thread, and the flat form is the one shape
   * where that costs it the most.  Nesting hands it the threaded form
   * directly: one `exp == 0x7FF` test per operand, and a zero test that hides
   * behind `exp == 0` instead of two booleans -- 9 instructions on the common
   * path where the flat form spent 28.  Measured under QEMU (icount, the whole
   * bench_double.c dadd kernel): tcc -9.6%, and gcc -2.2% as well, so this is
   * not a shape that merely suits one compiler.
   *
   * The nesting also drops work the ordering already makes redundant.  Once
   * the NaN cases have returned, a maximal exponent can only be an infinity,
   * so the mantissa test disappears from both infinity checks; and `b`'s
   * NaN-vs-infinity distinction only matters when `a` is an infinity, because
   * every other maximal-exponent `b` returns `b_bits` either way. */

  /* NaN and infinity: one `exp == 0x7FF` test per operand. */
  if (a_exp == 0x7FF)
  {
    if (a_mant != 0)
    {
      ur.u = a_bits; /* a is NaN */
      return ur.d;
    }
    /* a is an infinity. */
    if (b_exp == 0x7FF)
    {
      if (b_mant != 0)
      {
        ur.u = b_bits; /* b is NaN */
        return ur.d;
      }
      if (a_sign != b_sign)
      {
        ur.u = 0x7FF8000000000000ULL; /* inf + (-inf) = NaN */
        return ur.d;
      }
    }
    ur.u = a_bits;
    return ur.d;
  }
  if (b_exp == 0x7FF)
  {
    /* NaN or infinity, and `a` is finite: b_bits is the answer either way. */
    ur.u = b_bits;
    return ur.d;
  }

  /* Zero.  Only an operand whose exponent is 0 can be one, so the mantissa
   * test hides behind that -- which also leaves the two implicit-bit diamonds
   * below adjacent and if-convertible.
   *
   * IEEE 754 6.3: when both operands are zero, the result is +0 unless both
   * are negative (round-to-nearest mode). */
  if (a_exp == 0 && a_mant == 0)
  {
    if (b_exp == 0 && b_mant == 0)
    {
      ur.u = (a_sign && b_sign) ? DOUBLE_SIGN_BIT : 0;
      return ur.d;
    }
    ur.u = b_bits;
    return ur.d;
  }
  if (b_exp == 0 && b_mant == 0)
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
   * the exactly representable -(2^53-1) (gcc-torture ieee/pr28634).
   *
   * The sticky bit asks "did the shift drop a set bit", and the obvious way
   * to write that -- `m & ((1ULL << n) - 1)` -- is the expensive one on a
   * 32-bit machine.  Building the 64-bit mask costs eleven instructions (a
   * variable 64-bit shift of 1, then a 64-bit subtract), the AND costs two
   * more, and the mask plus its AND result plus `m` plus the shifted `m`
   * hold EIGHT registers at once, one more than dadd can spare here: the
   * allocator spilled the shifted value to the frame and reloaded it three
   * instructions later.  Shifting the dropped bits off the top instead
   * (`m << (64 - n)`, and `n` is 1..63 on this path so the count is never
   * 0 or 64) needs no mask and one register pair fewer. */
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
      b_mant = (b_mant >> exp_diff) | ((b_mant << (64 - exp_diff)) != 0);
    else
      b_mant = (b_mant != 0);
    result_exp = a_exp;
  }
  else if (exp_diff < 0)
  {
    /* b has larger exponent */
    if (-exp_diff < 64)
      a_mant = (a_mant >> -exp_diff) | ((a_mant << (64 + exp_diff)) != 0);
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
