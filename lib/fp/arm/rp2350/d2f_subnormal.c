/*
 * RP2350: the subnormal tail of __aeabi_d2f.
 *
 * The DCP's double->float conversion (WXUP / NRDF / RDFG, the same sequence
 * pico-sdk's dcp_double2float_m uses) flushes any result that does not fit the
 * normal float range to zero.  Measured on the device: (float)0x1.0p-148 came
 * back 0x00000000 instead of 0x00000001.
 *
 * That is not only a runtime defect -- gcc.c-torture ieee/20000320-1 converts
 * doubles whose float images are subnormal, and ieee/cdivchkf is built entirely
 * from subnormal hex float literals -- it also corrupts the *self-hosted
 * compiler*, because tccpp.c narrows every hex float literal with `tokc.f =
 * (float)d`.  Running on the RP2350 that cast is this function, so a subnormal
 * literal was already zero by the time it reached codegen; the cross compiler,
 * doing the same cast on the host, got it right.  One fix covers both.
 *
 * dcp_aeabi.S keeps the three-instruction DCP path for every exponent whose
 * result is normal, infinite or exactly zero, and tail-calls here only for
 * biased double exponents 1..896 (unbiased -1022..-127), where the float image
 * is subnormal or zero.  Inf/NaN and zero therefore cannot reach this code.
 *
 * Deliberately written in 32-bit halves rather than reusing dconv.c's
 * sfp_shr_sticky64()/sfp_round_pack_float(): this object is compiled by tcc
 * itself, and the first version -- which did use them -- returned 0 for every
 * input on device while being bit-exact when built with gcc.  Nothing here is
 * wider than a uint32_t, so there are no 64-bit shift helpers to get wrong.
 */

#include "../../fp_abi.h"

typedef union
{
  double d;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} d2f_double_words;

typedef union
{
  float f;
  uint32_t u;
} d2f_float_word;

float __rp2350_d2f_subnormal(double a)
{
  d2f_double_words ua;
  d2f_float_word ur;
  uint32_t hi;
  uint32_t lo;
  uint32_t sign;
  uint32_t sig_hi; /* significand bits 32..52, implicit bit included */
  uint32_t mant;   /* the float's 23-bit subnormal mantissa */
  uint32_t guard;
  uint32_t sticky;
  int exp;
  int k;
  int j;

  ua.d = a;
  hi = ua.w.hi;
  lo = ua.w.lo;
  sign = hi & 0x80000000u;
  exp = (int)((hi >> 20) & 0x7ffu);

  /* A float subnormal is M * 2^-149, and the value here is
   * (2^52 + mantissa) * 2^(exp-1023-52), so M is the significand shifted right
   * by k = 926 - exp.  exp is 1..896, hence k is 30..925. */
  k = 926 - exp;

  sig_hi = (hi & 0x000fffffu) | 0x00100000u;

  if (k >= 55)
  {
    /* Below half of the smallest subnormal: rounds to zero either way, and
     * bailing here keeps every shift below C's undefined 32-bit boundary. */
    mant = 0;
    guard = 0;
    sticky = 0;
  }
  else if (k >= 33)
  {
    /* The guard bit sits in the high word; the whole low word is sticky. */
    j = k - 32;
    mant = sig_hi >> j;
    guard = (sig_hi >> (j - 1)) & 1u;
    sticky = ((sig_hi & ((1u << (j - 1)) - 1u)) != 0u) || (lo != 0u);
  }
  else if (k == 32)
  {
    mant = sig_hi;
    guard = lo >> 31;
    sticky = (lo & 0x7fffffffu) != 0u;
  }
  else
  {
    /* k is 30 or 31: the result straddles the word boundary. */
    mant = (sig_hi << (32 - k)) | (lo >> k);
    guard = (lo >> (k - 1)) & 1u;
    sticky = (lo & ((1u << (k - 1)) - 1u)) != 0u;
  }

  /* Round to nearest, ties to even. */
  if (guard && (sticky || (mant & 1u)))
  {
    mant++;
  }

  /* mant can reach 2^23, and 0x00800000 is exactly the smallest normal float,
   * so the carry out of the subnormal field lands on the right encoding with
   * no special case. */
  ur.u = sign | mant;
  return ur.f;
}
