/*
 * ARM EABI Floating Point ABI Common Header
 * Defines structures and constants for FP operations across different FPU implementations
 */

#ifndef FP_ABI_H
#define FP_ABI_H

#include <stdint.h>

/* IEEE 754 single-precision float representation */
typedef union
{
  float f;
  uint32_t u;
  int32_t s;
  struct
  {
    uint32_t mantissa : 23;
    uint32_t exponent : 8;
    uint32_t sign : 1;
  } parts;
} float_bits;

/* IEEE 754 double-precision float representation */
typedef union
{
  double d;
  uint64_t u;
  int64_t s;
  struct
  {
    uint64_t mantissa : 52;
    uint64_t exponent : 11;
    uint64_t sign : 1;
  } parts;
} double_bits;

/* ARM EABI comparison result flags (returned in r0) */
#define AEABI_CMP_LT 0x0        /* a < b: Z=0, C=0 */
#define AEABI_CMP_EQ 0x40000000 /* a == b: Z=1 */
#define AEABI_CMP_GT 0x20000000 /* a > b: C=1, Z=0 */
#define AEABI_CMP_UN 0x80000000 /* unordered (NaN): N=1 */

/* Special float values */
#define FLOAT_SIGN_BIT 0x80000000
#define FLOAT_EXPONENT_MASK 0x7F800000
#define FLOAT_MANTISSA_MASK 0x007FFFFF
#define FLOAT_QUIET_BIT 0x00400000

/* Special double values */
#define DOUBLE_SIGN_BIT 0x8000000000000000ULL
#define DOUBLE_EXPONENT_MASK 0x7FF0000000000000ULL
#define DOUBLE_MANTISSA_MASK 0x000FFFFFFFFFFFFFULL
#define DOUBLE_QUIET_BIT 0x0008000000000000ULL

/* Exponent bias constants */
#define FLOAT_EXPONENT_BIAS 127
#define DOUBLE_EXPONENT_BIAS 1023

/* Special exponent values */
#define FLOAT_EXPONENT_INF 0xFF
#define DOUBLE_EXPONENT_INF 0x7FF

/* Helper macros */
#define FLOAT_IS_NAN(x) (((x).u & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK && ((x).u & FLOAT_MANTISSA_MASK) != 0)
#define FLOAT_IS_INF(x) (((x).u & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK && ((x).u & FLOAT_MANTISSA_MASK) == 0)
#define FLOAT_IS_ZERO(x) (((x).u & ~FLOAT_SIGN_BIT) == 0)

#define DOUBLE_IS_NAN(x) (((x).u & DOUBLE_EXPONENT_MASK) == DOUBLE_EXPONENT_MASK && ((x).u & DOUBLE_MANTISSA_MASK) != 0)
#define DOUBLE_IS_INF(x) (((x).u & DOUBLE_EXPONENT_MASK) == DOUBLE_EXPONENT_MASK && ((x).u & DOUBLE_MANTISSA_MASK) == 0)
#define DOUBLE_IS_ZERO(x) (((x).u & ~DOUBLE_SIGN_BIT) == 0)

#endif /* FP_ABI_H */
