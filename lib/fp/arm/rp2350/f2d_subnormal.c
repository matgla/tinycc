/*
 * RP2350: the subnormal case of __aeabi_f2d.
 *
 * The DCP's float->double conversion (WXYU / NRDD / RDDG, the sequence
 * pico-sdk's dcp_float2double_m uses) flushes a subnormal float to zero.
 * Measured on the device: (double)0x1p-149f came back 0.0 instead of 0x1p-149.
 *
 * Besides programs widening subnormal floats, it bent the self-hosted compiler:
 * tcc folds a float expression in double (`f1 = v1->c.f` in gen_opif), so on
 * the board every folded operation on a subnormal float constant read it as
 * zero -- -0x1p-149f came out -0.0f, where the cross compiler running on a PC
 * got 0x80000001 (Zig's compiler-rt exp2f/expf have that constant).
 *
 * dcp_aeabi.S keeps the DCP path for zero, normal, infinite and NaN floats and
 * tail-calls here only for a biased exponent of 0 with a nonzero mantissa.
 * Every subnormal float is a normal double, so this is exact: no rounding.
 * Written with 32-bit words only, like d2f_subnormal.c, for the same reason --
 * this object is compiled by tcc itself.
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
} f2d_double_words;

typedef union
{
  float f;
  uint32_t u;
} f2d_float_word;

double __rp2350_f2d_subnormal(float a)
{
  f2d_float_word ua;
  f2d_double_words ur;
  uint32_t mant;
  uint32_t rest;
  int p;
  int shift;

  ua.f = a;
  mant = ua.u & 0x007fffffu; /* nonzero: dcp_aeabi.S sends no zero here */

  /* The value is mant * 2^-149.  With its highest set bit at p (0..22) it is
   * 1.rest * 2^(p - 149): biased double exponent p + 874, and the p bits below
   * the leading one become the top of the 52-bit fraction, shifted up by
   * 52 - p (30..52). */
  p = 22;
  while (!(mant & (1u << p)))
    p--;
  rest = mant & ((1u << p) - 1u);
  shift = 52 - p;

  if (shift >= 32)
  {
    ur.w.hi = rest << (shift - 32);
    ur.w.lo = 0u;
  }
  else
  {
    /* p is 21 or 22: the fraction straddles the word boundary. */
    ur.w.hi = rest >> (32 - shift);
    ur.w.lo = rest << shift;
  }
  ur.w.hi |= (ua.u & 0x80000000u) | ((uint32_t)(p + 874) << 20);
  return ur.d;
}
