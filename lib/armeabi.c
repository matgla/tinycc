/*
 * TinyCC ARM EABI Runtime with Dynamic FP Library Selection
 *
 * This file provides the ARM EABI runtime support with dynamic selection
 * of floating point libraries based on compiler flags:
 * - -mfloat-abi: soft, softfp, hard
 * - -mfpu: vfpv4-sp-d16, fpv5-d16, none, etc.
 *
 * Dispatches to the appropriate FP library built from lib/fp/
 */

#include <stddef.h>

/* FP Library Selection
 * ====================
 *
 * The compiler flags determine which FP library is linked:
 *
 * -mfpu=none (soft float)          → lib/fp/libtcc1-fp-soft-$(TARGET).a
 * -mfpu=fpv4-sp-d16               → lib/fp/libtcc1-fp-vfpv4-sp-$(TARGET).a (float HW, double SW)
 * -mfpu=fpv5-d16                  → lib/fp/libtcc1-fp-vfpv5-dp-$(TARGET).a (both HW)
 * -mfpu=fpv5-sp-d16               → lib/fp/libtcc1-fp-vfpv4-sp-$(TARGET).a (float HW, double SW)
 * -DRP2350_DCP_ENABLED            → lib/fp/libtcc1-fp-rp2350-$(TARGET).a (double HW via DCP)
 *
 * Where $(TARGET) is the target architecture (e.g., armv8m, arm, etc.)
 *
 * The linker resolves __aeabi_* symbols from the selected library.
 * If multiple FP operations are needed (e.g., float HW + double SW),
 * multiple FP libraries can be linked in order.
 */

/* Non-floating point EABI functions remain in this file */

#if defined(__ARM_EABI__)

/* ARM EABI required symbols for non-FP operations */

/* Memory comparison functions required by EABI */
int __aeabi_memcpy_aligned(void *dest, const void *src, size_t n)
{
  /* stubbed */
  (void)dest;
  (void)src;
  (void)n;
  return 0;
}

int __aeabi_memcpy(void *dest, const void *src, size_t n)
{
  /* stubbed */
  (void)dest;
  (void)src;
  (void)n;
  return 0;
}

int __aeabi_memmove(void *dest, const void *src, size_t n)
{
  /* stubbed */
  (void)dest;
  (void)src;
  (void)n;
  return 0;
}

int __aeabi_memset(void *s, int c, size_t n)
{
  /* stubbed */
  (void)s;
  (void)c;
  (void)n;
  return 0;
}

int __aeabi_memclr(void *s, size_t n)
{
  /* stubbed */
  (void)s;
  (void)n;
  return 0;
}

/* Division functions */

/* Unsigned 32-bit division */
unsigned int __aeabi_uidiv(unsigned int numerator, unsigned int denominator)
{
  /* stubbed */
  (void)numerator;
  (void)denominator;
  return 0;
}

/* Signed 32-bit division */
int __aeabi_idiv(int numerator, int denominator)
{
  /* stubbed */
  (void)numerator;
  (void)denominator;
  return 0;
}

/* 64-bit unsigned division (returns quotient in r0:r1, remainder in r2:r3) */
typedef struct
{
  unsigned int quotient_low;
  unsigned int quotient_high;
  unsigned int remainder_low;
  unsigned int remainder_high;
} uint64_div_result;

int __aeabi_uldivmod(unsigned long long numerator, unsigned long long denominator)
{
#if 0
    /* TODO: implement full 64-bit unsigned division */
    uint64_div_result result;
    if (denominator == 0) {
        result.quotient_low = 0;
        result.quotient_high = 0;
        result.remainder_low = 0;
        result.remainder_high = 0;
        return result;
    }
    unsigned long long quotient = numerator / denominator;
    unsigned long long remainder = numerator % denominator;
    result.quotient_low = (unsigned int)quotient;
    result.quotient_high = (unsigned int)(quotient >> 32);
    result.remainder_low = (unsigned int)remainder;
    result.remainder_high = (unsigned int)(remainder >> 32);
    return result;
#else
  /* long long support not yet implemented */
  (void)numerator;
  (void)denominator;
  //   return (uint64_div_result){0, 0, 0, 0};
  return 0;
#endif
}

/* 64-bit signed division */
typedef struct
{
  int quotient_low;
  int quotient_high;
  int remainder_low;
  int remainder_high;
} int64_div_result;

int __aeabi_ldivmod(long long numerator, long long denominator)
{
#if 0
    /* TODO: implement full 64-bit signed division */
    int64_div_result result;
    if (denominator == 0) {
        result.quotient_low = 0;
        result.quotient_high = 0;
        result.remainder_low = 0;
        result.remainder_high = 0;
        return result;
    }
    long long quotient = numerator / denominator;
    long long remainder = numerator % denominator;
    result.quotient_low = (int)quotient;
    result.quotient_high = (int)(quotient >> 32);
    result.remainder_low = (int)remainder;
    result.remainder_high = (int)(remainder >> 32);
    return result;
#else
  /* long long support not yet implemented */
  (void)numerator;
  (void)denominator;
  //   return (int64_div_result){0, 0, 0, 0};
  return 0;
#endif
}

/* Unsigned 64-bit divide and return remainder */
unsigned long long __aeabi_ulmod(unsigned long long a, unsigned long long b)
{
#if 0
    /* TODO: implement full 64-bit unsigned modulus */
    if (b == 0) {
        return 0;
    }
    return a % b;
#else
  /* long long support not yet implemented */
  (void)a;
  (void)b;
  return 0;
#endif
}

/* Signed 64-bit divide and return remainder */
long long __aeabi_lmod(long long a, long long b)
{
#if 0
    /* TODO: implement full 64-bit signed modulus */
    if (b == 0) {
        return 0;
    }
    return a % b;
#else
  /* long long support not yet implemented */
  (void)a;
  (void)b;
  return 0;
#endif
}

/* Bit manipulation */

/* Count leading zeros */
int __aeabi_clz(int x)
{
  /* stubbed */
  (void)x;
  return 0;
}

/* 64-bit shift operations - soft implementations for ARM EABI */

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

/* Floating point conversions are provided by lib/fp/ libraries */

#endif /* __ARM_EABI__ */
