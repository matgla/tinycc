/* TCC ARM runtime EABI
   Copyright (C) 2013 Thomas Preud'homme

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.*/

#include <stddef.h>

/* Helper union for accessing double bits */
// typedef union {
//   double d;
//   unsigned long long ull;
//   struct {
//     unsigned int low;
//     unsigned int high;
//   } parts;
// } double_bits;

// /* Check if double is NaN - using 32-bit parts only */
// static int is_nan_d_parts(unsigned int high, unsigned int low) {
//   unsigned int exp = (high >> 20) & 0x7FF;
//   unsigned int frac_high = high & 0xFFFFF;
//   if (exp != 0x7FF)
//     return 0;
//   if (frac_high != 0)
//     return 1;
//   if (low != 0)
//     return 1;
//   return 0;
// }

// /* Bit-level double comparison using 32-bit parts only
//    returns -1 if a<b, 0 if a==b, 1 if a>b */
// static int dcmp_bits_parts(unsigned int a_high, unsigned int a_low,
//                            unsigned int b_high, unsigned int b_low) {
//   int sign_a = (a_high >> 31) & 1;
//   int sign_b = (b_high >> 31) & 1;

//   /* Handle zero cases - both +0.0 and -0.0 are equal */
//   unsigned int a_high_abs = a_high & 0x7FFFFFFF;
//   unsigned int b_high_abs = b_high & 0x7FFFFFFF;
//   if (a_high_abs == 0 && a_low == 0 && b_high_abs == 0 && b_low == 0) {
//     return 0;
//   }

//   /* Different signs */
//   if (sign_a != sign_b) {
//     if (sign_a)
//       return -1;
//     return 1;
//   }

//   /* Same sign - compare magnitude */
//   if (sign_a == 0) {
//     /* Both positive */
//     if (a_high > b_high)
//       return 1;
//     if (a_high < b_high)
//       return -1;
//     if (a_low > b_low)
//       return 1;
//     if (a_low < b_low)
//       return -1;
//     return 0;
//   }
//   /* Both negative - reverse comparison */
//   if (a_high > b_high)
//     return -1;
//   if (a_high < b_high)
//     return 1;
//   if (a_low > b_low)
//     return -1;
//   if (a_low < b_low)
//     return 1;
//   return 0;
// }

/* Double precision comparison functions */
int __aeabi_dcmpun(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 1;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 1;
  return 0;
}

int __aeabi_dcmple(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 0;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 0;
  // int cmp =
  //     dcmp_bits_parts(ba.parts.high, ba.parts.low, bb.parts.high,
  //     bb.parts.low);
  // if (cmp <= 0)
  //   return 1;
  return 0;
}

int __aeabi_dcmplt(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 0;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 0;
  // int cmp =
  //     dcmp_bits_parts(ba.parts.high, ba.parts.low, bb.parts.high,
  //     bb.parts.low);
  // if (cmp < 0)
  //   return 1;
  return 0;
}

int __aeabi_dcmpeq(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 0;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 0;
  // int cmp =
  //     dcmp_bits_parts(ba.parts.high, ba.parts.low, bb.parts.high,
  //     bb.parts.low);
  // if (cmp == 0)
  //   return 1;
  return 0;
}

int __aeabi_dcmpge(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 0;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 0;
  // int cmp =
  //     dcmp_bits_parts(ba.parts.high, ba.parts.low, bb.parts.high,
  //     bb.parts.low);
  // if (cmp >= 0)
  //   return 1;
  return 0;
}

int __aeabi_dcmpgt(double a, double b)
{
  // double_bits ba, bb;
  // ba.d = a;
  // bb.d = b;
  // if (is_nan_d_parts(ba.parts.high, ba.parts.low))
  //   return 0;
  // if (is_nan_d_parts(bb.parts.high, bb.parts.low))
  //   return 0;
  // int cmp =
  //     dcmp_bits_parts(ba.parts.high, ba.parts.low, bb.parts.high,
  //     bb.parts.low);
  // if (cmp > 0)
  //   return 1;
  return 0;
}

/* Single precision comparison stubs */
int __aeabi_fcmpun(float a, float b)
{
  return 0;
}
int __aeabi_fcmple(float a, float b)
{
  return 0;
}
int __aeabi_fcmplt(float a, float b)
{
  return 0;
}
int __aeabi_fcmpeq(float a, float b)
{
  return 0;
}
int __aeabi_fcmpge(float a, float b)
{
  return 0;
}
int __aeabi_fcmpgt(float a, float b)
{
  return 0;
}

/* These set CPSR flags directly (used by soft-float code) */
void __aeabi_cfcmple(float a, float b)
{
}
void __aeabi_cfcmpeq(float a, float b)
{
}
void __aeabi_cdcmple(double a, double b)
{
}
void __aeabi_cdcmpeq(double a, double b)
{
}

// typedef struct {
//   unsigned long long quot;
//   unsigned long long rem;
// } ulldiv_t;

int __aeabi_uldivmod(unsigned long long n, unsigned long long d)
{
  // return (ulldiv_t){
  // .quot = 0,
  // .rem = 0,
  // };
  return 0;
}

/* Double precision arithmetic stubs */
double __aeabi_dmul(double a, double b)
{
  return 0;
}
double __aeabi_dadd(double a, double b)
{
  return 0;
}
double __aeabi_dsub(double a, double b)
{
  return 0;
}
double __aeabi_ddiv(double a, double b)
{
  return 0;
}
double __aeabi_dneg(double a)
{
  return 0;
}

/* Single precision arithmetic stubs */
float __aeabi_fmul(float a, float b)
{
  return 0;
}
float __aeabi_fadd(float a, float b)
{
  return 0;
}
float __aeabi_fsub(float a, float b)
{
  return 0;
}
float __aeabi_fdiv(float a, float b)
{
  return 0;
}
float __aeabi_fneg(float a)
{
  return 0;
}

/* Helper union for accessing float bits */
typedef union
{
  float f;
  unsigned int ui;
} float_bits;

/* Conversion functions */

/* Double to int conversion */
int __aeabi_d2iz(double a)
{
  // double_bits da;
  // da.d = a;

  // /* Extract sign, exponent, mantissa */
  // int sign = (da.parts.high >> 31) & 1;
  // int exp = (da.parts.high >> 20) & 0x7FF;

  // /* Handle special cases */
  // if (exp == 0)
  //   return 0; /* Zero or denormal */
  // if (exp == 0x7FF)
  //   return 0; /* NaN or infinity */

  // /* Compute actual exponent */
  // int actual_exp = exp - 1023;

  // /* If exponent is negative, result is 0 */
  // if (actual_exp < 0)
  //   return 0;

  // /* If exponent is too large, overflow */
  // if (actual_exp > 30)
  //   return sign ? 0x80000000 : 0x7FFFFFFF;

  // /* Extract mantissa (52 bits) and add implicit 1 */
  // unsigned long long mantissa =
  //     ((unsigned long long)(da.parts.high & 0xFFFFF) << 32) | da.parts.low;
  // mantissa |= (1ULL << 52); /* Add implicit leading 1 */

  // /* Shift mantissa based on exponent */
  // int result;
  // if (actual_exp >= 52) {
  //   result = mantissa << (actual_exp - 52);
  // } else {
  //   result = mantissa >> (52 - actual_exp);
  // }

  // return sign ? -result : result;
  return 0;
}

/* Int to double conversion */
double __aeabi_i2d(int a)
{
  // double_bits result;

  // if (a == 0) {
  //   result.parts.high = 0;
  //   result.parts.low = 0;
  //   return result.d;
  // }

  // /* Handle sign */
  // int sign = 0;
  // unsigned int abs_val = a;
  // if (a < 0) {
  //   sign = 1;
  //   abs_val = -a;
  // }

  // /* Find the highest set bit */
  // int shift = 0;
  // unsigned int temp = abs_val;
  // while (temp > 1) {
  //   temp >>= 1;
  //   shift++;
  // }

  // /* Compute exponent (biased by 1023) */
  // int exp = shift + 1023;

  // /* Compute mantissa (52 bits, without implicit 1) */
  // unsigned long long mantissa;
  // if (shift >= 52) {
  //   mantissa =
  //       ((unsigned long long)abs_val >> (shift - 52)) & 0xFFFFFFFFFFFFFULL;
  // } else {
  //   mantissa =
  //       ((unsigned long long)abs_val << (52 - shift)) & 0xFFFFFFFFFFFFFULL;
  // }

  // /* Pack into double */
  // result.parts.high = (sign << 31) | (exp << 20) | ((mantissa >> 32) &
  // 0xFFFFF); result.parts.low = mantissa & 0xFFFFFFFF;

  // return result.d;
  return 0;
}

/* Unsigned int to double conversion */
double __aeabi_ui2d(unsigned int a)
{
  // double_bits result;

  // if (a == 0) {
  //   result.parts.high = 0;
  //   result.parts.low = 0;
  //   return result.d;
  // }

  // /* Find the highest set bit */
  // int shift = 0;
  // unsigned int temp = a;
  // while (temp > 1) {
  //   temp >>= 1;
  //   shift++;
  // }

  // /* Compute exponent (biased by 1023) */
  // int exp = shift + 1023;

  // /* Compute mantissa (52 bits, without implicit 1) */
  // unsigned long long mantissa;
  // if (shift >= 52) {
  //   mantissa = ((unsigned long long)a >> (shift - 52)) & 0xFFFFFFFFFFFFFULL;
  // } else {
  //   mantissa = ((unsigned long long)a << (52 - shift)) & 0xFFFFFFFFFFFFFULL;
  // }

  // /* Pack into double */
  // result.parts.high = (exp << 20) | ((mantissa >> 32) & 0xFFFFF);
  // result.parts.low = mantissa & 0xFFFFFFFF;

  // return result.d;
  return 0;
}

/* Float to int conversion */
int __aeabi_f2iz(float a)
{
  // float_bits fa;
  // fa.f = a;

  // /* Extract sign, exponent, mantissa */
  // int sign = (fa.ui >> 31) & 1;
  // int exp = (fa.ui >> 23) & 0xFF;

  // /* Handle special cases */
  // if (exp == 0)
  //   return 0; /* Zero or denormal */
  // if (exp == 0xFF)
  //   return 0; /* NaN or infinity */

  // /* Compute actual exponent */
  // int actual_exp = exp - 127;

  // /* If exponent is negative, result is 0 */
  // if (actual_exp < 0)
  //   return 0;

  // /* If exponent is too large, overflow */
  // if (actual_exp > 30)
  //   return sign ? 0x80000000 : 0x7FFFFFFF;

  // /* Extract mantissa (23 bits) and add implicit 1 */
  // unsigned int mantissa = (fa.ui & 0x7FFFFF) | 0x800000;

  // /* Shift mantissa based on exponent */
  // int result;
  // if (actual_exp >= 23) {
  //   result = mantissa << (actual_exp - 23);
  // } else {
  //   result = mantissa >> (23 - actual_exp);
  // }

  // return sign ? -result : result;
  return 0;
}

/* Int to float conversion */
float __aeabi_i2f(int a)
{
  // float_bits result;

  // if (a == 0) {
  //   result.ui = 0;
  //   return result.f;
  // }

  // /* Handle sign */
  // int sign = 0;
  // unsigned int abs_val = a;
  // if (a < 0) {
  //   sign = 1;
  //   abs_val = -a;
  // }

  // /* Find the highest set bit */
  // int shift = 0;
  // unsigned int temp = abs_val;
  // while (temp > 1) {
  //   temp >>= 1;
  //   shift++;
  // }

  // /* Compute exponent (biased by 127) */
  // int exp = shift + 127;

  // /* Compute mantissa (23 bits, without implicit 1) */
  // unsigned int mantissa;
  // if (shift >= 23) {
  //   mantissa = (abs_val >> (shift - 23)) & 0x7FFFFF;
  // } else {
  //   mantissa = (abs_val << (23 - shift)) & 0x7FFFFF;
  // }

  // /* Pack into float */
  // result.ui = (sign << 31) | (exp << 23) | mantissa;

  // return result.f;
  return 0;
}

/* Unsigned int to float conversion */
float __aeabi_ui2f(unsigned int a)
{
  // float_bits result;

  // if (a == 0) {
  //   result.ui = 0;
  //   return result.f;
  // }

  // /* Find the highest set bit */
  // int shift = 0;
  // unsigned int temp = a;
  // while (temp > 1) {
  //   temp >>= 1;
  //   shift++;
  // }

  // /* Compute exponent (biased by 127) */
  // int exp = shift + 127;

  // /* Compute mantissa (23 bits, without implicit 1) */
  // unsigned int mantissa;
  // if (shift >= 23) {
  //   mantissa = (a >> (shift - 23)) & 0x7FFFFF;
  // } else {
  //   mantissa = (a << (23 - shift)) & 0x7FFFFF;
  // }

  // /* Pack into float */
  // result.ui = (exp << 23) | mantissa;

  // return result.f;s
  return 0;
}

/* Double to float conversion */
float __aeabi_d2f(double a)
{
  // double_bits da;
  // float_bits result;
  // da.d = a;

  // /* Extract sign, exponent, mantissa from double */
  // int sign = (da.parts.high >> 31) & 1;
  // int exp = (da.parts.high >> 20) & 0x7FF;
  // unsigned long long mantissa =
  //     ((unsigned long long)(da.parts.high & 0xFFFFF) << 32) | da.parts.low;

  // /* Handle special cases */
  // if (exp == 0) {
  //   /* Zero or denormal */
  //   result.ui = sign << 31;
  //   return result.f;
  // }
  // if (exp == 0x7FF) {
  //   /* NaN or infinity */
  //   if (mantissa != 0) {
  //     result.ui = (sign << 31) | 0x7FC00000; /* NaN */
  //   } else {
  //     result.ui = (sign << 31) | 0x7F800000; /* Infinity */
  //   }
  //   return result.f;
  // }

  // /* Convert exponent from double (bias 1023) to float (bias 127) */
  // int new_exp = exp - 1023 + 127;

  // /* Check for overflow/underflow */
  // if (new_exp >= 0xFF) {
  //   /* Overflow to infinity */
  //   result.ui = (sign << 31) | 0x7F800000;
  //   return result.f;
  // }
  // if (new_exp <= 0) {
  //   /* Underflow to zero */
  //   result.ui = sign << 31;
  //   return result.f;
  // }

  // /* Convert mantissa from 52 bits to 23 bits */
  // unsigned int new_mantissa = (mantissa >> 29) & 0x7FFFFF;

  // /* Pack into float */
  // result.ui = (sign << 31) | (new_exp << 23) | new_mantissa;

  // return result.f;
  return 0;
}

/* Float to double conversion */
double __aeabi_f2d(float a)
{
  // float_bits fa;
  // double_bits result;
  // fa.f = a;

  // /* Extract sign, exponent, mantissa from float */
  // int sign = (fa.ui >> 31) & 1;
  // int exp = (fa.ui >> 23) & 0xFF;
  // unsigned int mantissa = fa.ui & 0x7FFFFF;

  // /* Handle special cases */
  // if (exp == 0) {
  //   /* Zero or denormal */
  //   result.parts.high = sign << 31;
  //   result.parts.low = 0;
  //   return result.d;
  // }
  // if (exp == 0xFF) {
  //   /* NaN or infinity */
  //   result.parts.high = (sign << 31) | (0x7FF << 20);
  //   if (mantissa != 0) {
  //     result.parts.high |= 0x80000; /* NaN */
  //     result.parts.low = 0;
  //   } else {
  //     result.parts.low = 0; /* Infinity */
  //   }
  //   return result.d;
  // }

  // /* Convert exponent from float (bias 127) to double (bias 1023) */
  // int new_exp = exp - 127 + 1023;

  // /* Convert mantissa from 23 bits to 52 bits */
  // unsigned long long new_mantissa = ((unsigned long long)mantissa) << 29;

  // /* Pack into double */
  // result.parts.high =
  //     (sign << 31) | (new_exp << 20) | ((new_mantissa >> 32) & 0xFFFFF);
  // result.parts.low = new_mantissa & 0xFFFFFFFF;

  // return result.d;
  return 0;
}

double __aeabi_idivmod(int a, int b)
{
  return 0;
}

void __aeabi_memset(void *dest, int n, int c)
{
  for (int i = 0; i < n; i++)
  {
    ((unsigned char *)dest)[i] = (unsigned char)c;
  }
}

void __aeabi_memmove8(void *dest, int n, int c)
{
}

// #ifdef __TINYC__
// #define INT_MIN (-2147483647 - 1)
// #define INT_MAX 2147483647
// #define UINT_MAX 0xffffffff
// #define LONG_MIN (-2147483647L - 1)
// #define LONG_MAX 2147483647L
// #define ULONG_MAX 0xffffffffUL
// #define LLONG_MAX 9223372036854775807LL
// #define LLONG_MIN (-9223372036854775807LL - 1)
// #define ULLONG_MAX 0xffffffffffffffffULL
// #else
// #include <limits.h>
// #endif

// /* We rely on the little endianness and EABI calling convention for this to
//    work */

// typedef struct double_unsigned_struct {
//   unsigned low;
//   unsigned high;
// } double_unsigned_struct;

// typedef struct unsigned_int_struct {
//   unsigned low;
//   int high;
// } unsigned_int_struct;

// #define REGS_RETURN(name, type) \
//   void name##_return(type ret) {}

// /* Float helper functions */

// #define FLOAT_EXP_BITS 8
// #define FLOAT_FRAC_BITS 23

// #define DOUBLE_EXP_BITS 11
// #define DOUBLE_FRAC_BITS 52

// #define ONE_EXP(type) ((1 << (type##_EXP_BITS - 1)) - 1)

// REGS_RETURN(unsigned_int_struct, unsigned_int_struct)
// REGS_RETURN(double_unsigned_struct, double_unsigned_struct)

// /* float -> integer: (sign) 1.fraction x 2^(exponent - exp_for_one) */

// // /* float to [unsigned] long long conversion */
// // #define DEFINE__AEABI_F2XLZ(name, with_sign) \
// //   void __aeabi_##name(unsigned val) { \
// //     int exp, high_shift, sign; \
// //     double_unsigned_struct ret; \
// // \
// //     /* compute sign */ \
// //     sign = val >> 31; \
// // \
// //     /* compute real exponent */ \
// //     exp = val >> FLOAT_FRAC_BITS; \
// //     exp &= (1 << FLOAT_EXP_BITS) - 1; \
// //     exp -= ONE_EXP(FLOAT); \
// // \
// //     /* undefined behavior if truncated value cannot be represented */ \
// //     if (with_sign) { \
// //       if (exp > 62) /* |val| too big, double cannot represent LLONG_MAX */
// \
// //         return; \
// //     } else { \
// //       if ((sign && exp >= 0) || exp > 63) /* if val < 0 || val too big */
// \
// //         return; \
// //     } \
// // \
// //     val &= (1 << FLOAT_FRAC_BITS) - 1; \
// //     if (exp >= 32) { \
// //       ret.high = 1 << (exp - 32); \
// //       if (exp - 32 >= FLOAT_FRAC_BITS) { \
// //         ret.high |= val << (exp - 32 - FLOAT_FRAC_BITS); \
// //         ret.low = 0; \
// //       } else { \
// //         high_shift = FLOAT_FRAC_BITS - (exp - 32); \
// //         ret.high |= val >> high_shift; \
// //         ret.low = val << (32 - high_shift); \
// //       } \
// //     } else { \
// //       ret.high = 0; \
// //       ret.low = 1 << exp; \
// //       if (exp > FLOAT_FRAC_BITS) \
// //         ret.low |= val << (exp - FLOAT_FRAC_BITS); \
// //       else \
// //         ret.low |= val >> (FLOAT_FRAC_BITS - exp); \
// //     } \
// // \
// //     /* encode negative integer using 2's complement */ \
// //     if (with_sign && sign) { \
// //       ret.low = ~ret.low; \
// //       ret.high = ~ret.high; \
// //       if (ret.low == UINT_MAX) { \
// //         ret.low = 0; \
// //         ret.high++; \
// //       } else \
// //         ret.low++; \
// //     } \
// // \
// //     double_unsigned_struct_return(ret); \
// //   }

// /* float to unsigned long long conversion */
// // DEFINE__AEABI_F2XLZ(f2ulz, 0)

// /* float to long long conversion */
// // DEFINE__AEABI_F2XLZ(f2lz, 1)

// /* double to [unsigned] long long conversion */
// // #define DEFINE__AEABI_D2XLZ(name, with_sign) \
// //   void __aeabi_##name(double_unsigned_struct val) { \
// //     int exp, high_shift, sign; \
// //     double_unsigned_struct ret; \
// // \
// //     if ((val.high & ~0x80000000) == 0 && val.low == 0) { \
// //       ret.low = ret.high = 0; \
// //       goto _ret_; \
// //     } \
// // \
// //     /* compute sign */ \
// //     sign = val.high >> 31; \
// // \
// //     /* compute real exponent */ \
// //     exp = (val.high >> (DOUBLE_FRAC_BITS - 32)); \
// //     exp &= (1 << DOUBLE_EXP_BITS) - 1; \
// //     exp -= ONE_EXP(DOUBLE); \
// // \
// //     /* undefined behavior if truncated value cannot be represented */ \
// //     if (with_sign) { \
// //       if (exp > 62) /* |val| too big, double cannot represent LLONG_MAX */
// \
// //         return; \
// //     } else { \
// //       if ((sign && exp >= 0) || exp > 63) /* if val < 0 || val too big */
// \
// //         return; \
// //     } \
// // \
// //     val.high &= (1 << (DOUBLE_FRAC_BITS - 32)) - 1; \
// //     if (exp >= 32) { \
// //       ret.high = 1 << (exp - 32); \
// //       if (exp >= DOUBLE_FRAC_BITS) { \
// //         high_shift = exp - DOUBLE_FRAC_BITS; \
// //         ret.high |= val.high << high_shift; \
// //         ret.high |= val.low >> (32 - high_shift); \
// //         ret.low = val.low << high_shift; \
// //       } else { \
// //         high_shift = DOUBLE_FRAC_BITS - exp; \
// //         ret.high |= val.high >> high_shift; \
// //         ret.low = val.high << (32 - high_shift); \
// //         ret.low |= val.low >> high_shift; \
// //       } \
// //     } else { \
// //       ret.high = 0; \
// //       ret.low = 1 << exp; \
// //       if (exp > DOUBLE_FRAC_BITS - 32) { \
// //         high_shift = exp - DOUBLE_FRAC_BITS - 32; \
// //         ret.low |= val.high << high_shift; \
// //         ret.low |= val.low >> (32 - high_shift); \
// //       } else \
// //         ret.low |= val.high >> (DOUBLE_FRAC_BITS - 32 - exp); \
// //     } \
// // \
// //     /* encode negative integer using 2's complement */ \
// //     if (with_sign && sign) { \
// //       ret.low = ~ret.low; \
// //       ret.high = ~ret.high; \
// //       if (ret.low == UINT_MAX) { \
// //         ret.low = 0; \
// //         ret.high++; \
// //       } else \
// //         ret.low++; \
// //     } \
// // \
// //   _ret_: \
// //     double_unsigned_struct_return(ret); \
// //   }

// /* double to unsigned long long conversion */
// DEFINE__AEABI_D2XLZ(d2ulz, 0)

// /* double to long long conversion */
// DEFINE__AEABI_D2XLZ(d2lz, 1)

// /* long long to float conversion */
// #define DEFINE__AEABI_XL2F(name, with_sign) \
//   unsigned __aeabi_##name(unsigned long long v) { \
//     int s /* shift */, flb /* first lost bit */, sign = 0; \
//     unsigned p = 0 /* power */, ret; \
//     double_unsigned_struct val; \
//                                                                                \
//     /* fraction in negative float is encoded in 1's complement */ \
//     if (with_sign && (v & (1ULL << 63))) { \
//       sign = 1; \
//       v = ~v + 1; \
//     } \
//     val.low = v; \
//     val.high = v >> 32; \
//     /* fill fraction bits */ \
//     for (s = 31, p = 1 << 31; p && !(val.high & p); s--, p >>= 1) \
//       ; \
//     if (p) { \
//       ret = val.high & (p - 1); \
//       if (s < FLOAT_FRAC_BITS) { \
//         ret <<= FLOAT_FRAC_BITS - s; \
//         ret |= val.low >> (32 - (FLOAT_FRAC_BITS - s)); \
//         flb = (val.low >> (32 - (FLOAT_FRAC_BITS - s - 1))) & 1; \
//       } else { \
//         flb = (ret >> (s - FLOAT_FRAC_BITS - 1)) & 1; \
//         ret >>= s - FLOAT_FRAC_BITS; \
//       } \
//       s += 32; \
//     } else { \
//       for (s = 31, p = 1 << 31; p && !(val.low & p); s--, p >>= 1) \
//         ; \
//       if (p) { \
//         ret = val.low & (p - 1); \
//         if (s <= FLOAT_FRAC_BITS) { \
//           ret <<= FLOAT_FRAC_BITS - s; \
//           flb = 0; \
//         } else { \
//           flb = (ret >> (s - FLOAT_FRAC_BITS - 1)) & 1; \
//           ret >>= s - FLOAT_FRAC_BITS; \
//         } \
//       } else \
//         return 0; \
//     } \
//     if (flb) \
//       ret++; \
//                                                                                \
//     /* fill exponent bits */ \
//     ret |= (s + ONE_EXP(FLOAT)) << FLOAT_FRAC_BITS; \
//                                                                                \
//     /* fill sign bit */ \
//     ret |= sign << 31; \
//                                                                                \
//     return ret; \
//   }

// /* unsigned long long to float conversion */
// DEFINE__AEABI_XL2F(ul2f, 0)

// /* long long to float conversion */
// DEFINE__AEABI_XL2F(l2f, 1)

// /* long long to double conversion */
// #define __AEABI_XL2D(name, with_sign) \
//   void __aeabi_##name(unsigned long long v) { \
//     int s /* shift */, high_shift, sign = 0; \
//     unsigned tmp, p = 0; \
//     double_unsigned_struct val, ret; \
//                                                                                \
//     /* fraction in negative float is encoded in 1's complement */ \
//     if (with_sign && (v & (1ULL << 63))) { \
//       sign = 1; \
//       v = ~v + 1; \
//     } \
//     val.low = v; \
//     val.high = v >> 32; \
//                                                                                \
//     /* fill fraction bits */ \
//     for (s = 31, p = 1 << 31; p && !(val.high & p); s--, p >>= 1) \
//       ; \
//     if (p) { \
//       tmp = val.high & (p - 1); \
//       if (s < DOUBLE_FRAC_BITS - 32) { \
//         high_shift = DOUBLE_FRAC_BITS - 32 - s; \
//         ret.high = tmp << high_shift; \
//         ret.high |= val.low >> (32 - high_shift); \
//         ret.low = val.low << high_shift; \
//       } else { \
//         high_shift = s - (DOUBLE_FRAC_BITS - 32); \
//         ret.high = tmp >> high_shift; \
//         ret.low = tmp << (32 - high_shift); \
//         ret.low |= val.low >> high_shift; \
//         if ((val.low >> (high_shift - 1)) & 1) { \
//           if (ret.low == UINT_MAX) { \
//             ret.high++; \
//             ret.low = 0; \
//           } else \
//             ret.low++; \
//         } \
//       } \
//       s += 32; \
//     } else { \
//       for (s = 31, p = 1 << 31; p && !(val.low & p); s--, p >>= 1) \
//         ; \
//       if (p) { \
//         tmp = val.low & (p - 1); \
//         if (s <= DOUBLE_FRAC_BITS - 32) { \
//           high_shift = DOUBLE_FRAC_BITS - 32 - s; \
//           ret.high = tmp << high_shift; \
//           ret.low = 0; \
//         } else { \
//           high_shift = s - (DOUBLE_FRAC_BITS - 32); \
//           ret.high = tmp >> high_shift; \
//           ret.low = tmp << (32 - high_shift); \
//         } \
//       } else { \
//         ret.high = ret.low = 0; \
//         goto _ret_; \
//       } \
//     } \
//                                                                                \
//     /* fill exponent bits */ \
//     ret.high |= (s + ONE_EXP(DOUBLE)) << (DOUBLE_FRAC_BITS - 32); \
//                                                                                \
//     /* fill sign bit */ \
//     ret.high |= sign << 31; \
//                                                                                \
//   _ret_: \
//     double_unsigned_struct_return(ret); \
//   }

// /* unsigned long long to double conversion */
// __AEABI_XL2D(ul2d, 0)

// /* long long to double conversion */
// __AEABI_XL2D(l2d, 1)

// /* Long long helper functions */

// /* TODO: add error in case of den == 0 (see §4.3.1 and §4.3.2) */

// #define define_aeabi_xdivmod_signed_type(basetype, type) \
//   typedef struct type { \
//     basetype quot; \
//     unsigned basetype rem; \
//   } type

// #define define_aeabi_xdivmod_unsigned_type(basetype, type) \
//   typedef struct type { \
//     basetype quot; \
//     basetype rem; \
//   } type

// #define AEABI_UXDIVMOD(name, type, rettype, typemacro) \
//   static inline rettype aeabi_##name(type num, type den) { \
//     rettype ret; \
//     type quot = 0; \
//                                                                                \
//     /* Increase quotient while it is less than numerator */ \
//     while (num >= den) { \
//       type q = 1; \
//                                                                                \
//       /* Find closest power of two */ \
//       while ((q << 1) * den <= num && q * den <= typemacro##_MAX / 2) \
//         q <<= 1; \
//                                                                                \
//       /* Compute difference between current quotient and numerator */ \
//       num -= q * den; \
//       quot += q; \
//     } \
//     ret.quot = quot; \
//     ret.rem = num; \
//     return ret; \
//   }

// #define __AEABI_XDIVMOD(name, type, uiname, rettype, urettype, typemacro) \
//   void __aeabi_##name(type numerator, type denominator) { \
//     unsigned type num, den; \
//     urettype uxdiv_ret; \
//     rettype ret; \
//                                                                                \
//     if (numerator >= 0) \
//       num = numerator; \
//     else \
//       num = 0 - numerator; \
//     if (denominator >= 0) \
//       den = denominator; \
//     else \
//       den = 0 - denominator; \
//     uxdiv_ret = aeabi_##uiname(num, den); \
//     /* signs differ */ \
//     if ((numerator & typemacro##_MIN) != (denominator & typemacro##_MIN)) \
//       ret.quot = 0 - uxdiv_ret.quot; \
//     else \
//       ret.quot = uxdiv_ret.quot; \
//     if (numerator < 0) \
//       ret.rem = 0 - uxdiv_ret.rem; \
//     else \
//       ret.rem = uxdiv_ret.rem; \
//                                                                                \
//     rettype##_return(ret); \
//   }

// define_aeabi_xdivmod_signed_type(long long, lldiv_t);
// define_aeabi_xdivmod_unsigned_type(unsigned long long, ulldiv_t);
// define_aeabi_xdivmod_signed_type(int, idiv_t);
// define_aeabi_xdivmod_unsigned_type(unsigned, uidiv_t);

// REGS_RETURN(lldiv_t, lldiv_t)
// REGS_RETURN(ulldiv_t, ulldiv_t)
// REGS_RETURN(idiv_t, idiv_t)
// REGS_RETURN(uidiv_t, uidiv_t)

// AEABI_UXDIVMOD(uldivmod, unsigned long long, ulldiv_t, ULLONG)

// __AEABI_XDIVMOD(ldivmod, long long, uldivmod, lldiv_t, ulldiv_t, LLONG)

// void __aeabi_uldivmod(unsigned long long num, unsigned long long den) {
//   ulldiv_t_return(aeabi_uldivmod(num, den));
// }

// void __aeabi_llsl(double_unsigned_struct val, int shift) {
//   double_unsigned_struct ret;

//   if (shift >= 32) {
//     val.high = val.low;
//     val.low = 0;
//     shift -= 32;
//   }
//   if (shift > 0) {
//     ret.low = val.low << shift;
//     ret.high = (val.high << shift) | (val.low >> (32 - shift));
//     double_unsigned_struct_return(ret);
//     return;
//   }
//   double_unsigned_struct_return(val);
// }

// #define aeabi_lsr(val, shift, fill, type) \
//   type##_struct ret; \
//                                                                                \
//   if (shift >= 32) { \
//     val.low = val.high; \
//     val.high = fill; \
//     shift -= 32; \
//   } \
//   if (shift > 0) { \
//     ret.high = val.high >> shift; \
//     ret.low = (val.high << (32 - shift)) | (val.low >> shift); \
//     type##_struct_return(ret); \
//     return; \
//   } \ type##_struct_return(val);

// void __aeabi_llsr(double_unsigned_struct val, int shift) {
//   aeabi_lsr(val, shift, 0, double_unsigned);
// }

// void __aeabi_lasr(unsigned_int_struct val, int shift) {
//   aeabi_lsr(val, shift, val.high >> 31, unsigned_int);
// }

// /* Integer division functions */

// AEABI_UXDIVMOD(uidivmod, unsigned, uidiv_t, UINT)

// int __aeabi_idiv(int numerator, int denominator) {
//   unsigned num, den;
//   uidiv_t ret;

//   if (numerator >= 0)
//     num = numerator;
//   else
//     num = 0 - numerator;
//   if (denominator >= 0)
//     den = denominator;
//   else
//     den = 0 - denominator;
//   ret = aeabi_uidivmod(num, den);
//   if ((numerator & INT_MIN) != (denominator & INT_MIN)) /* signs differ */
//     ret.quot *= -1;
//   return ret.quot;
// }

// unsigned __aeabi_uidiv(unsigned num, unsigned den) {
//   return aeabi_uidivmod(num, den).quot;
// }

// __AEABI_XDIVMOD(idivmod, int, uidivmod, idiv_t, uidiv_t, INT)

// void __aeabi_uidivmod(unsigned num, unsigned den) {
//   uidiv_t_return(aeabi_uidivmod(num, den));
// }

// /* Some targets do not have all eabi calls (OpenBSD) */
// typedef __SIZE_TYPE__ size_t;
// extern void *memcpy(void *dest, const void *src, size_t n);
// extern void *memmove(void *dest, const void *src, size_t n);
// extern void *memset(void *s, int c, size_t n);

void *__aeabi_memcpy(void *dest, const void *src, size_t n)
{
  return memcpy(dest, src, n);
}

// void *__aeabi_memmove(void *dest, const void *src, size_t n) {
//   return memmove(dest, src, n);
// }

// void *__aeabi_memmove4(void *dest, const void *src, size_t n) {
//   return memmove(dest, src, n);
// }

// void *__aeabi_memmove8(void *dest, const void *src, size_t n) {
//   return memmove(dest, src, n);
// }

// void *__aeabi_memset(void *s, size_t n, int c) { return memset(s, c, n); }
