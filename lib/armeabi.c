/*
 * TinyCC ARM EABI Runtime with Dynamic FP Library Selection
 *
 * This file provides the ARM EABI runtime support with dynamic selection
 * of floating point libraries based on compiler flags:
 * - -mfloat-abi: soft, softfp, hard
 * - -mfpu: vfpv4-sp-d16, fpv5-d16, none, etc.
 *
 * Dispatches to the appropriate FP library built from lib/fp/
 *
 * KNOWN BUG WORKAROUND:
 * TinyCC ARM Thumb has a critical bug in the >= operator for unsigned comparisons.
 * Symptoms: (a >= b) returns incorrect values (often 0 when should be 1, or garbage).
 * Workaround: Replace (a >= b) with !(a < b) which works correctly.
 * See tests/ir_tests/test_ge_operator.c for test cases.
 */

#include <stddef.h>

typedef unsigned int u32;
typedef int s32;

static float aeabi_fneg_impl(float a)
{
  union
  {
    float f;
    u32 u;
  } v;

  v.f = a;
  v.u ^= 0x80000000u;
  return v.f;
}

static double aeabi_dneg_impl(double a)
{
  union
  {
    double d;
    unsigned long long u;
  } v;

  v.d = a;
  v.u ^= 0x8000000000000000ULL;
  return v.d;
}

/* FP Library Selection
 * ====================
 *
 * The compiler flags determine which FP library is linked:
 *
 * -mfpu=none (soft float)          → libsoftfp.{a,so}
 * -mfpu=fpv4-sp-d16               → libvfpv4sp.{a,so} (float HW, double SW)
 * -mfpu=fpv5-d16                  → libvfpv5dp.{a,so} (both HW)
 * -mfpu=fpv5-sp-d16               → libvfpv4sp.{a,so} (float HW, double SW)
 * -DRP2350_DCP_ENABLED            → librp2350fp.{a,so} (double HW via DCP)
 *
 * tcc_add_library() searches for .so first, then .a in library paths.
 * The linker resolves __aeabi_* symbols from the selected library.
 * If multiple FP operations are needed (e.g., float HW + double SW),
 * multiple FP libraries can be linked in order.
 */

/* Non-floating point EABI functions remain in this file */

#if defined(__ARM_EABI__)

/* ARM EABI required symbols for non-FP operations */

/* Memory functions required by EABI.  ARMv8-M takes them from arm_mem.S
 * (LDM/STM blocks); these byte/word loops remain for the other ARM targets. */
#if !defined(__ARM_ARCH_8M__)

/* NOTE: ARM EABI defines __aeabi_memset() argument order as (dest, n, c),
 * i.e. it differs from ISO C memset(dest, c, n).
 */

static void *aeabi_memcpy_impl(void *dest, const void *src, size_t n)
{
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;

  /* memcpy has undefined behavior for overlap; we still implement a simple
   * forward copy (fast and correct for non-overlapping ranges).
   */
  if (n == 0 || d == s)
    return dest;

  /* If both pointers are word-aligned, copy words first. */
  {
    unsigned long da = (unsigned long)d;
    unsigned long sa = (unsigned long)s;
    if (((da | sa) & (sizeof(unsigned long) - 1)) == 0)
    {
      unsigned long *dw = (unsigned long *)d;
      const unsigned long *sw = (const unsigned long *)s;
      while (n >= sizeof(unsigned long))
      {
        *dw++ = *sw++;
        n -= sizeof(unsigned long);
      }
      d = (unsigned char *)dw;
      s = (const unsigned char *)sw;
    }
  }

  while (n--)
    *d++ = *s++;
  return dest;
}

static void *aeabi_memmove_impl(void *dest, const void *src, size_t n)
{
  unsigned char *d = (unsigned char *)dest;
  const unsigned char *s = (const unsigned char *)src;

  if (n == 0 || d == s)
    return dest;

  if (d < s || d >= (s + n))
  {
    /* Non-overlapping (or forward-safe overlap) */
    return aeabi_memcpy_impl(dest, src, n);
  }

  /* Overlap with dest inside source range: copy backwards. */
  d += n;
  s += n;
  while (n--)
    *--d = *--s;
  return dest;
}

void *__aeabi_memcpy_aligned(void *dest, const void *src, size_t n)
{
  /* Caller promises alignment; our impl already takes advantage of it. */
  return aeabi_memcpy_impl(dest, src, n);
}

void *__aeabi_memcpy(void *dest, const void *src, size_t n)
{
  return aeabi_memcpy_impl(dest, src, n);
}

void *__aeabi_memmove(void *dest, const void *src, size_t n)
{
  return aeabi_memmove_impl(dest, src, n);
}

/* ARM EABI convenience entrypoint: src/dest are 4-byte aligned and n is a
 * multiple of 4. Some generated code calls this symbol directly.
 */
void *__aeabi_memmove4(void *dest, const void *src, size_t n)
{
  return aeabi_memmove_impl(dest, src, n);
}

/* ARM EABI convenience entrypoint: src/dest are 8-byte aligned and n is a
 * multiple of 8. TCC generates calls to this for 8-byte aligned struct copies.
 */
void *__aeabi_memmove8(void *dest, const void *src, size_t n)
{
  return aeabi_memmove_impl(dest, src, n);
}

void *__aeabi_memset(void *dest, size_t n, int c)
{
  unsigned char *d = (unsigned char *)dest;
  unsigned char byte = (unsigned char)c;

  if (n == 0)
    return dest;

  /* If word-aligned, expand byte to a word and store words first. */
  {
    unsigned long da = (unsigned long)d;
    if ((da & (sizeof(unsigned long) - 1)) == 0)
    {
      unsigned long pattern = 0;
      for (unsigned i = 0; i < sizeof(unsigned long); ++i)
        pattern = (pattern << 8) | byte;

      unsigned long *dw = (unsigned long *)d;
      while (n >= sizeof(unsigned long))
      {
        *dw++ = pattern;
        n -= sizeof(unsigned long);
      }
      d = (unsigned char *)dw;
    }
  }

  while (n--)
    *d++ = byte;
  return dest;
}

void __aeabi_memclr(void *dest, size_t n)
{
  (void)__aeabi_memset(dest, n, 0);
}

#endif /* !__ARM_ARCH_8M__ */

/* Division functions */

/* Unsigned 32-bit division */
unsigned int __aeabi_uidiv(unsigned int numerator, unsigned int denominator)
{
  /* Simple restoring division (avoids libgcc dependency). */
  if (denominator == 0)
    return 0;
  u32 q = 0;
  u32 r = 0;
  for (int i = 31; i >= 0; --i)
  {
    r = (r << 1) | ((numerator >> i) & 1u);
    /* Workaround for >= bug: use !(r < denominator) instead of (r >= denominator) */
    if (!(r < denominator))
    {
      r -= denominator;
      q |= (1u << i);
    }
  }
  return q;
}

/* Signed 32-bit division */
int __aeabi_idiv(int numerator, int denominator)
{
  if (denominator == 0)
    return 0;
  int neg = 0;
  u32 un = (u32)numerator;
  u32 ud = (u32)denominator;
  if (numerator < 0)
  {
    neg ^= 1;
    un = (u32)(-numerator);
  }
  if (denominator < 0)
  {
    neg ^= 1;
    ud = (u32)(-denominator);
  }
  u32 q = __aeabi_uidiv(un, ud);
  return neg ? -(int)q : (int)q;
}

/* 64-bit unsigned division/modulus.
 * AAPCS returns small structs in r0-r3; TCC also consumes quotient in r0:r1.
 *
 * TinyCC (ARM/Thumb) currently miscompiles functions that *return* a 16-byte
 * struct, using an implicit sret pointer, which does not match the EABI for
 * __aeabi_{u,}ldivmod (which returns quotient in r0:r1 and remainder in r2:r3).
 * We therefore implement the EABI entry points in assembly (armeabi_divmod.S)
 * and call these C helpers to compute the results into memory.
 *
 * The whole divide — the clz normalization, the two-digit (Hacker's Delight
 * divlu) step and the 64/64 estimate — is inline in the unsigned helper, so
 * a 64-bit divide pays one stack frame and no out-of-line helper calls.
 * The chain this replaces (helper -> udivmod_u64 -> divlu -> armeabi_clz32)
 * cost core-sort's 4,560 timestamp divides 569k instructions in the tcc
 * kernel against the llvm kernel's 260k through compiler_rt's single
 * __udivmoddi4.  Divide by zero gives 0 remainder 0; the bit-at-a-time loop
 * this family replaces took ~2,600 instructions a call.
 */
void __tcc_aeabi_uldivmod_helper(u32 n_lo, u32 n_hi, u32 d_lo, u32 d_hi, u32 *q_lo, u32 *q_hi, u32 *r_lo, u32 *r_hi)
{
  u32 ql = 0, qh = 0, rl = 0, rh = 0;
  u32 v, vn1, vn0, un32, un21, un10, un1, un0, q1, q0, rhat, k, u1, u0, p_lo, p_hi, rem_lo, rem_hi;
  int s;

  if ((d_lo | d_hi) != 0)
  {
    if (d_hi == 0)
    {
      /* d fits 32 bits: peel the high part, then the two-digit step on
       * (k:n_lo): normalise v, estimate each 16-bit quotient digit by
       * dividing by v's top 16 bits (UDIV on ARMv8-M), correct at most
       * twice. */
      k = n_hi;
      if (n_hi >= d_lo)
      {
        qh = n_hi / d_lo;
        k = n_hi - qh * d_lo;
      }

      v = d_lo;
      s = 0;
      if ((v & 0xFFFF0000u) == 0)
      {
        s += 16;
        v <<= 16;
      }
      if ((v & 0xFF000000u) == 0)
      {
        s += 8;
        v <<= 8;
      }
      if ((v & 0xF0000000u) == 0)
      {
        s += 4;
        v <<= 4;
      }
      if ((v & 0xC0000000u) == 0)
      {
        s += 2;
        v <<= 2;
      }
      if ((v & 0x80000000u) == 0)
      {
        s += 1;
        v <<= 1;
      }

      vn1 = v >> 16;
      vn0 = v & 0xFFFFu;
      un32 = s ? (k << s) | (n_lo >> (32 - s)) : k;
      un10 = n_lo << s;
      un1 = un10 >> 16;
      un0 = un10 & 0xFFFFu;

      q1 = un32 / vn1;
      rhat = un32 - q1 * vn1;
      while (q1 > 0xFFFFu || q1 * vn0 > ((rhat << 16) | un1))
      {
        q1--;
        rhat += vn1;
        if (rhat > 0xFFFFu)
          break;
      }
      un21 = (un32 << 16) + un1 - q1 * v;

      q0 = un21 / vn1;
      rhat = un21 - q0 * vn1;
      while (q0 > 0xFFFFu || q0 * vn0 > ((rhat << 16) | un0))
      {
        q0--;
        rhat += vn1;
        if (rhat > 0xFFFFu)
          break;
      }

      rl = ((un21 << 16) + un0 - q0 * v) >> s;
      ql = (q1 << 16) | q0;
    }
    else if (n_hi < d_hi || (n_hi == d_hi && n_lo < d_lo))
    {
      rl = n_lo;
      rh = n_hi;
    }
    else
    {
      /* d >= 2^32: the quotient fits 32 bits.  Estimate it from n / 2 over
       * the top 32 bits of d normalised; the estimate is exact or one too
       * big. */
      v = d_hi;
      s = 0;
      if ((v & 0xFFFF0000u) == 0)
      {
        s += 16;
        v <<= 16;
      }
      if ((v & 0xFF000000u) == 0)
      {
        s += 8;
        v <<= 8;
      }
      if ((v & 0xF0000000u) == 0)
      {
        s += 4;
        v <<= 4;
      }
      if ((v & 0xC0000000u) == 0)
      {
        s += 2;
        v <<= 2;
      }
      if ((v & 0x80000000u) == 0)
      {
        s += 1;
        v <<= 1;
      }
      if (s)
        v |= d_lo >> (32 - s);

      /* v's top bit is set, so the two-digit step needs no normalisation
       * of its own. */
      u1 = n_hi >> 1;
      u0 = (n_lo >> 1) | (n_hi << 31);
      vn1 = v >> 16;
      vn0 = v & 0xFFFFu;
      un32 = u1;
      un10 = u0;
      un1 = un10 >> 16;
      un0 = un10 & 0xFFFFu;

      q1 = un32 / vn1;
      rhat = un32 - q1 * vn1;
      while (q1 > 0xFFFFu || q1 * vn0 > ((rhat << 16) | un1))
      {
        q1--;
        rhat += vn1;
        if (rhat > 0xFFFFu)
          break;
      }
      un21 = (un32 << 16) + un1 - q1 * v;

      q0 = un21 / vn1;
      rhat = un21 - q0 * vn1;
      while (q0 > 0xFFFFu || q0 * vn0 > ((rhat << 16) | un0))
      {
        q0--;
        rhat += vn1;
        if (rhat > 0xFFFFu)
          break;
      }

      q0 = ((q1 << 16) | q0) >> (31 - s);
      if (q0 != 0)
        q0--;

      /* 32 x 32 -> 64 multiply from 16-bit halves (32-bit operations only). */
      {
        u32 al = q0 & 0xFFFFu, ah = q0 >> 16, bl = d_lo & 0xFFFFu, bh = d_lo >> 16;
        u32 ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
        u32 mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
        p_lo = (ll & 0xFFFFu) | (mid << 16);
        p_hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
      }
      p_hi += q0 * d_hi;
      rem_lo = n_lo - p_lo;
      rem_hi = n_hi - p_hi - (n_lo < p_lo);
      if (rem_hi > d_hi || (rem_hi == d_hi && rem_lo >= d_lo))
      {
        q0++;
        rem_hi = rem_hi - d_hi - (rem_lo < d_lo);
        rem_lo -= d_lo;
      }

      ql = q0;
      rl = rem_lo;
      rh = rem_hi;
    }
  }

  *q_lo = ql;
  *q_hi = qh;
  *r_lo = rl;
  *r_hi = rh;
}

/* Type definitions for 64-bit operations */
typedef unsigned int Wtype;
typedef long long DWtype;
typedef unsigned long long UDWtype;

struct DWstruct
{
  Wtype low, high;
};

typedef union
{
  struct DWstruct s;
  DWtype ll;
} DWunion;

static inline void u64_neg(u32 *lo, u32 *hi)
{
  *lo = ~(*lo) + 1u;
  *hi = ~(*hi) + (*lo == 0);
}

void __tcc_aeabi_ldivmod_helper(u32 n_lo, s32 n_hi, u32 d_lo, s32 d_hi, u32 *q_lo, u32 *q_hi, u32 *r_lo, u32 *r_hi)
{
  int q_neg = 0;
  int r_neg = 0;

  u32 un_lo = n_lo;
  u32 un_hi = (u32)n_hi;
  u32 ud_lo = d_lo;
  u32 ud_hi = (u32)d_hi;

  if (n_hi < 0)
  {
    q_neg ^= 1;
    r_neg = 1;
    u64_neg(&un_lo, &un_hi);
  }
  if (d_hi < 0)
  {
    q_neg ^= 1;
    u64_neg(&ud_lo, &ud_hi);
  }

  __tcc_aeabi_uldivmod_helper(un_lo, un_hi, ud_lo, ud_hi, q_lo, q_hi, r_lo, r_hi);

  if (q_neg)
    u64_neg(q_lo, q_hi);
  if (r_neg)
    u64_neg(r_lo, r_hi);
}

/* 64-bit comparison functions */

/* Signed 64-bit comparison
 * Returns: <0 if a < b, 0 if a == b, >0 if a > b
 * Uses only 32-bit operations to avoid recursive long long comparison.
 *
 * NOTE: We use explicit 32-bit parameters instead of long long because
 * TinyCC ARM Thumb has a compiler bug where assigning 64-bit function
 * parameters to local variables can generate incorrect code that stores
 * the wrong register pair (stores r0:r1 instead of r2:r3 for the second
 * parameter). Using explicit 32-bit parameters avoids this bug. */
int __aeabi_lcmp(unsigned int a_lo, int a_hi, unsigned int b_lo, int b_hi)
{
  /* Compare high words first (signed) */
  if (a_hi < b_hi)
  {
    return -1;
  }
  if (a_hi > b_hi)
  {
    return 1;
  }
  /* High words equal, compare low words (unsigned) */
  if (a_lo < b_lo)
  {
    return -1;
  }
  if (a_lo > b_lo)
  {
    return 1;
  }

  return 0;
}

/* Unsigned 64-bit comparison
 * Returns: <0 if a < b, 0 if a == b, >0 if a > b
 * Uses only 32-bit operations to avoid recursive long long comparison */
int __aeabi_ulcmp(unsigned int a_lo, unsigned int a_hi, unsigned int b_lo, unsigned int b_hi)
{
  /* Compare high words first (unsigned) */
  if (a_hi < b_hi)
    return -1;
  if (a_hi > b_hi)
    return 1;
  /* High words equal, compare low words (unsigned) */
  if (a_lo < b_lo)
    return -1;
  if (a_lo > b_lo)
    return 1;
  return 0;
}

/* Bit manipulation */

/* Count leading zeros */
int __aeabi_clz(int x)
{
  /* Portable clz for 32-bit (undefined for x==0 per EABI; return 32). */
  u32 v = (u32)x;
  if (v == 0)
    return 32;
  int n = 0;
  for (u32 bit = 0x80000000u; (v & bit) == 0; bit >>= 1)
    ++n;
  return n;
}

/* 64-bit shift operations - soft implementations for ARM EABI */

/* Logical shift right for 64-bit unsigned */
unsigned long long __aeabi_llsr(unsigned long long a, int b)
{
  DWunion u;
  u.ll = a;
  if (b >= 32)
  {
    u.s.low = (unsigned)u.s.high >> (b - 32);
    u.s.high = 0;
  }
  else if (b != 0)
  {
    u.s.low = ((unsigned)u.s.low >> b) | (u.s.high << (32 - b));
    u.s.high = (unsigned)u.s.high >> b;
  }
  return u.ll;
}

/* Arithmetic shift left for 64-bit signed */
long long __aeabi_llsl(long long a, int b)
{
  DWunion u;
  u.ll = a;
  if (b >= 32)
  {
    u.s.high = (unsigned)u.s.low << (b - 32);
    u.s.low = 0;
  }
  else if (b != 0)
  {
    u.s.high = ((unsigned)u.s.high << b) | ((unsigned)u.s.low >> (32 - b));
    u.s.low = (unsigned)u.s.low << b;
  }
  return u.ll;
}

/* Arithmetic shift right for 64-bit signed */
long long __aeabi_lasr(long long a, int b)
{
  DWunion u;
  u.ll = a;
  if (b >= 32)
  {
    u.s.low = (u32)((s32)u.s.high >> (b - 32));
    u.s.high = (s32)u.s.high >> 31;
  }
  else if (b != 0)
  {
    u.s.low = ((u32)u.s.low >> b) | ((u32)u.s.high << (32 - b));
    u.s.high = (s32)u.s.high >> b;
  }
  return u.ll;
}

__attribute__((weak)) float __aeabi_fneg(float a)
{
  return aeabi_fneg_impl(a);
}

/* Count leading zeros in 32-bit value */
static int armeabi_clz32(u32 x)
{
  int n = 0;
  if (x == 0)
    return 32;
  if ((x & 0xFFFF0000u) == 0)
  {
    n += 16;
    x <<= 16;
  }
  if ((x & 0xFF000000u) == 0)
  {
    n += 8;
    x <<= 8;
  }
  if ((x & 0xF0000000u) == 0)
  {
    n += 4;
    x <<= 4;
  }
  if ((x & 0xC0000000u) == 0)
  {
    n += 2;
    x <<= 2;
  }
  if ((x & 0x80000000u) == 0)
  {
    n += 1;
  }
  return n;
}

/* Find MSB position (0-63) for a non-zero 64-bit value */
static int armeabi_msb64(unsigned long long a)
{
  u32 hi = (u32)(a >> 32);
  if (hi != 0)
    return 63 - armeabi_clz32(hi);
  return 31 - armeabi_clz32((u32)a);
}

/*
 * Pure bit-manipulation IEEE 754 conversions with round-to-nearest-even.
 * These MUST NOT use float/double casts (TCC would turn those into recursive
 * calls back to these very functions).
 */

float __aeabi_ul2f(unsigned long long a)
{
  union
  {
    float f;
    u32 u;
  } r;

  if (a == 0)
  {
    r.u = 0;
    return r.f;
  }

  int msb = armeabi_msb64(a);
  int exp = 127 + msb;

  if (msb <= 23)
  {
    /* Exact: value fits in 24-bit mantissa */
    u32 mant = ((u32)a << (23 - msb)) & 0x7FFFFFu;
    r.u = ((u32)exp << 23) | mant;
    return r.f;
  }

  /* Need rounding */
  int shift = msb - 23;
  u32 mant = (u32)(a >> shift);

  /* IEEE 754 round-to-nearest-even */
  unsigned long long dropped_mask = (1ULL << shift) - 1;
  unsigned long long dropped = a & dropped_mask;
  unsigned long long half = 1ULL << (shift - 1);

  if (dropped > half || (dropped == half && (mant & 1)))
  {
    mant++;
    if (mant == (1u << 24))
    {
      mant = (1u << 23);
      exp++;
    }
  }

  mant &= 0x7FFFFFu;
  r.u = ((u32)exp << 23) | mant;
  return r.f;
}

double __aeabi_ul2d(unsigned long long a)
{
  union
  {
    double d;
    unsigned long long u;
  } r;

  if (a == 0)
  {
    r.u = 0;
    return r.d;
  }

  int msb = armeabi_msb64(a);
  int exp = 1023 + msb;

  if (msb <= 52)
  {
    /* Exact: value fits in 53-bit mantissa */
    unsigned long long mant = (a << (52 - msb)) & 0xFFFFFFFFFFFFFULL;
    r.u = ((unsigned long long)exp << 52) | mant;
    return r.d;
  }

  /* Need rounding */
  int shift = msb - 52;
  unsigned long long mant = a >> shift;

  /* IEEE 754 round-to-nearest-even */
  unsigned long long dropped_mask = (1ULL << shift) - 1;
  unsigned long long dropped = a & dropped_mask;
  unsigned long long half = 1ULL << (shift - 1);

  if (dropped > half || (dropped == half && (mant & 1)))
  {
    mant++;
    if (mant == (1ULL << 53))
    {
      mant = (1ULL << 52);
      exp++;
    }
  }

  mant &= 0xFFFFFFFFFFFFFULL;
  r.u = ((unsigned long long)exp << 52) | mant;
  return r.d;
}

float __aeabi_l2f(long long a)
{
  union
  {
    float f;
    u32 u;
  } r;

  if (a == 0)
  {
    r.u = 0;
    return r.f;
  }

  u32 sign = 0;
  unsigned long long mag;
  if (a < 0)
  {
    sign = 0x80000000u;
    mag = -(unsigned long long)a;
  }
  else
  {
    mag = (unsigned long long)a;
  }

  int msb = armeabi_msb64(mag);
  int exp = 127 + msb;

  if (msb <= 23)
  {
    u32 mant = ((u32)mag << (23 - msb)) & 0x7FFFFFu;
    r.u = sign | ((u32)exp << 23) | mant;
    return r.f;
  }

  int shift = msb - 23;
  u32 mant = (u32)(mag >> shift);

  unsigned long long dropped_mask = (1ULL << shift) - 1;
  unsigned long long dropped = mag & dropped_mask;
  unsigned long long half = 1ULL << (shift - 1);

  if (dropped > half || (dropped == half && (mant & 1)))
  {
    mant++;
    if (mant == (1u << 24))
    {
      mant = (1u << 23);
      exp++;
    }
  }

  mant &= 0x7FFFFFu;
  r.u = sign | ((u32)exp << 23) | mant;
  return r.f;
}

double __aeabi_l2d(long long a)
{
  union
  {
    double d;
    unsigned long long u;
  } r;

  if (a == 0)
  {
    r.u = 0;
    return r.d;
  }

  unsigned long long sign = 0;
  unsigned long long mag;
  if (a < 0)
  {
    sign = 0x8000000000000000ULL;
    mag = -(unsigned long long)a;
  }
  else
  {
    mag = (unsigned long long)a;
  }

  int msb = armeabi_msb64(mag);
  int exp = 1023 + msb;

  if (msb <= 52)
  {
    unsigned long long mant = (mag << (52 - msb)) & 0xFFFFFFFFFFFFFULL;
    r.u = sign | ((unsigned long long)exp << 52) | mant;
    return r.d;
  }

  int shift = msb - 52;
  unsigned long long mant = mag >> shift;

  unsigned long long dropped_mask = (1ULL << shift) - 1;
  unsigned long long dropped = mag & dropped_mask;
  unsigned long long half = 1ULL << (shift - 1);

  if (dropped > half || (dropped == half && (mant & 1)))
  {
    mant++;
    if (mant == (1ULL << 53))
    {
      mant = (1ULL << 52);
      exp++;
    }
  }

  mant &= 0xFFFFFFFFFFFFFULL;
  r.u = sign | ((unsigned long long)exp << 52) | mant;
  return r.d;
}

#endif /* __ARM_EABI__ */
