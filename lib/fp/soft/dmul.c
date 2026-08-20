/*
 * Soft-float Multiplication - Double Precision
 * Implements __aeabi_dmul for ARM EABI
 * Pure software IEEE 754 implementation - no FPU required
 */

#include "../fp_abi.h"
#include "soft_common.h"

/* 64x64 -> 128 multiply, as four 32x32->64 partial products.
 *
 * This used to split each 32x32 into 16-bit halves and accumulate in 32-bit
 * words with explicit carry, to dodge armv8m codegen that was unreliable for
 * 64-bit add/adc and for shifts by 32.  That is no longer true: the backend
 * emits UMULL for a widening 32x32 multiply and ADDS/ADC for a 64-bit add,
 * both verified against the FP conformance vectors.  The hand decomposition
 * cost ~20 instructions per partial product where UMULL is one, and this is
 * the hot path -- a dynamic profile of the benchmark's dmul kernel puts
 * 87,576 calls through here against 5,000 through the classifiers.
 *
 * Only two 64-bit idioms appear below, both of which lower cleanly: extracting
 * a high word, `(uint32_t)(x >> 32)`, and composing one, `(uint64_t)hi << 32`.
 */
static inline void mul64wide(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
  /* Extract the 32-bit words by shift/truncate, not via a u64_words union
   * local: the armv8m cross drops the union's 64-bit store and then reads the
   * high word from uninitialised stack (a partial-read aliasing miscompile
   * that survives even -O0).  Direct casts are codegen-correct here. */
  const uint32_t a0 = (uint32_t)a, a1 = (uint32_t)(a >> 32);
  const uint32_t b0 = (uint32_t)b, b1 = (uint32_t)(b >> 32);

  const uint64_t p00 = (uint64_t)a0 * b0;
  const uint64_t p01 = (uint64_t)a0 * b1;
  const uint64_t p10 = (uint64_t)a1 * b0;
  const uint64_t p11 = (uint64_t)a1 * b1;

  /* The 2^32 column: three 32-bit addends, so it needs 34 bits and cannot be
   * held in a uint32_t.  Its carry-out feeds the 2^64 column. */
  const uint64_t mid = (uint64_t)(uint32_t)(p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;

  *lo = ((uint64_t)(uint32_t)mid << 32) | (uint32_t)p00;
  /* Cannot overflow: p11 <= (2^32-1)^2 leaves exactly enough headroom for the
   * two high halves and the carry (max total 2^64-1). */
  *hi = p11 + (uint32_t)(p01 >> 32) + (uint32_t)(p10 >> 32) + (uint32_t)(mid >> 32);
}

/* Multiply two double-precision floats */
double __aeabi_dmul(double a, double b)
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

  /* Result sign is XOR of input signs */
  int result_sign = a_sign ^ b_sign;

  /* NaN, infinity and zero, nested rather than written as a flat sequence of
   * independent `if`s.
   *
   * Flat, this read `is_nan_parts(a)`, `is_nan_parts(b)`, `is_inf_parts(a)`,
   * `is_inf_parts(b)`, `is_zero_parts(a) || is_zero_parts(b)` -- which tests
   * `a_exp == 0x7FF` twice and `b_exp == 0x7FF` twice, and each of those needs
   * a `movw` to materialize 0x7FF before the compare.  Every re-test is
   * decided by an earlier branch on the same value, so a compiler that threads
   * jumps through duplicated test blocks removes them; tcc does not thread.
   * Nesting hands it the threaded form: one maximal-exponent test per operand
   * on the common path.  See the same rewrite in dadd.c. */
  if (a_exp == 0x7FF)
  {
    if (a_mant != 0)
    {
      ur.u = a_bits; /* a is NaN */
      return ur.d;
    }
    /* a is an infinity; a NaN b still wins, as in the flat order. */
    if (b_exp == 0x7FF && b_mant != 0)
    {
      ur.u = b_bits; /* b is NaN */
      return ur.d;
    }
    if (b_exp == 0 && b_mant == 0)
    {
      ur.u = 0x7FF8000000000000ULL; /* inf * 0 = NaN */
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }
  if (b_exp == 0x7FF)
  {
    if (b_mant != 0)
    {
      ur.u = b_bits; /* b is NaN */
      return ur.d;
    }
    /* b is an infinity and a is finite. */
    if (a_exp == 0 && a_mant == 0)
    {
      ur.u = 0x7FF8000000000000ULL; /* 0 * inf = NaN */
      return ur.d;
    }
    ur.u = make_double(result_sign, 0x7FF, 0);
    return ur.d;
  }

  /* Zero.  Neither operand is a NaN or an infinity here, so the mantissa test
   * only runs for the operands whose exponent is already known to be 0. */
  if ((a_exp == 0 && a_mant == 0) || (b_exp == 0 && b_mant == 0))
  {
    ur.u = make_double(result_sign, 0, 0);
    return ur.d;
  }

  /* Fast path: multiplying by an exact power-of-two keeps the other mantissa
   * unchanged (no rounding), only the exponent is adjusted.
   *
   * This also avoids low-opt codegen pitfalls in the wide-multiply path.
   */
  if (a_exp != 0 && b_exp != 0)
  {
    int exp = a_exp + b_exp - DOUBLE_EXP_BIAS;

    /* Only when the result is a normal number.  Out of range the shortcut is
     * not actually a shortcut: an underflowing result still has to be shifted
     * into the subnormal range and rounded there (this path used to flush it
     * to zero, so DBL_MIN * 0.5 returned 0 instead of DBL_MIN/2), and an
     * overflowing one still has to round before deciding on infinity.  Both
     * are exactly what the general path below does. */
    if (exp > 0 && exp < 0x7FF)
    {
      if (a_mant == 0)
      {
        ur.u = make_double(result_sign, exp, b_mant);
        return ur.d;
      }
      if (b_mant == 0)
      {
        ur.u = make_double(result_sign, exp, a_mant);
        return ur.d;
      }
    }
  }

  /* Add implicit bit for normalized numbers.
   *
   * Subnormals are normalized here instead: their effective exponent is 1 and
   * their leading bit sits below bit 52, which would break the "leading 1 of
   * the product is at bit 104 or 105" invariant the shift below relies on.
   * Scaling them into the normal form (and paying for it in the exponent,
   * which may go negative) keeps the whole 106-bit path unchanged. */
  if (a_exp != 0)
  {
    a_mant |= DOUBLE_IMPLICIT_BIT;
  }
  else
  {
    a_exp = 1;
    while (!(a_mant & DOUBLE_IMPLICIT_BIT))
    {
      a_mant <<= 1;
      a_exp--;
    }
  }
  if (b_exp != 0)
  {
    b_mant |= DOUBLE_IMPLICIT_BIT;
  }
  else
  {
    b_exp = 1;
    while (!(b_mant & DOUBLE_IMPLICIT_BIT))
    {
      b_mant <<= 1;
      b_exp--;
    }
  }

  /* Calculate result exponent: ea + eb - bias */
  int result_exp = a_exp + b_exp - DOUBLE_EXP_BIAS;

  /* Multiply mantissas (53-bit * 53-bit = up to 106-bit result).
   * Mantissas are integer values with the implicit bit set at bit 52.
   * The raw product therefore has its leading 1 at bit 104 or 105.
   */
  uint64_t prod_hi, prod_lo;
  mul64wide(a_mant, b_mant, &prod_hi, &prod_lo);

  /* Normalize so the implicit bit ends up at bit 52.
   * If bit105 is set, shift by 53 and increment exponent.
   * Otherwise shift by 52.
   */
  /* Determine whether the top bit is at position 105 (vs 104). Avoid 64-bit
   * masking/shift here; use 32-bit word access instead.
   *
   * bit105 is bit 41 within prod_hi, i.e. bit 9 of prod_hi.hi (bits 32..63).
   */
  int shift = 52;
  if (((uint32_t)(prod_hi >> 32)) & (1u << 9))
  {
    shift = 53;
    result_exp++;
  }

  /* Compute mant = prod >> shift (yields a 53-bit value with implicit bit).
   *
   * Do this with 32-bit pieces to avoid fragile 64-bit shift codegen on some
   * low-opt paths.
   */
  const uint32_t prod_lo_lo = (uint32_t)prod_lo;
  const uint32_t prod_lo_hi = (uint32_t)(prod_lo >> 32);
  const uint32_t prod_hi_lo = (uint32_t)prod_hi;
  const uint32_t prod_hi_hi = (uint32_t)(prod_hi >> 32);

  uint32_t mant_lo32;
  uint32_t mant_hi32;
  int guard;
  int sticky;
  if (shift == 52)
  {
    /* mant = (prod_hi << 12) | (prod_lo >> 52) */
    mant_lo32 = (prod_hi_lo << 12) | (prod_lo_hi >> 20);
    mant_hi32 = (prod_hi_hi << 12) | (prod_hi_lo >> 20);

    /* guard is bit 51 of prod_lo => bit 19 of prod_lo_hi */
    guard = (int)((prod_lo_hi >> 19) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 19) - 1u)) != 0);
  }
  else
  {
    /* shift == 53: mant = (prod_hi << 11) | (prod_lo >> 53) */
    mant_lo32 = (prod_hi_lo << 11) | (prod_lo_hi >> 21);
    mant_hi32 = (prod_hi_hi << 11) | (prod_hi_lo >> 21);

    /* guard is bit 52 of prod_lo => bit 20 of prod_lo_hi */
    guard = (int)((prod_lo_hi >> 20) & 1u);
    sticky = (prod_lo_lo != 0) || ((prod_lo_hi & ((1u << 20) - 1u)) != 0);
  }

  uint64_t mant = ((uint64_t)mant_hi32 << 32) | (uint64_t)mant_lo32;

  /* Hand the significand plus its guard/sticky bits to the shared rounding
   * core.  Rounding must happen *after* any shift into the subnormal range,
   * otherwise a result that underflows is rounded at full precision and then
   * truncated -- and the previous code rounded first and then flushed every
   * result with result_exp <= 0 to zero outright.
   *
   * guard occupies bit 2 and sticky bit 0 of the SFP_GRS field, which makes
   * "guard && (sticky || lsb)" exactly the nearest-even rule the core applies. */
  {
    uint64_t mant_grs = (mant << SFP_GRS) | ((uint64_t)(guard != 0) << (SFP_GRS - 1)) | (uint64_t)(sticky != 0);
    ur.u = sfp_round_pack_double(result_sign, result_exp, mant_grs);
    return ur.d;
  }
}
