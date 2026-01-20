#include <stdint.h>

extern int putchar(int c);

extern double __aeabi_dadd(double a, double b);
extern double __aeabi_dsub(double a, double b);
extern double __aeabi_dmul(double a, double b);
extern double __aeabi_ddiv(double a, double b);
extern double __aeabi_dneg(double a);

extern int __aeabi_dcmpeq(double a, double b);
extern int __aeabi_dcmplt(double a, double b);
extern int __aeabi_dcmple(double a, double b);
extern int __aeabi_dcmpgt(double a, double b);
extern int __aeabi_dcmpge(double a, double b);
extern int __aeabi_dcmpun(double a, double b);

extern int __aeabi_d2iz(double a);
extern unsigned int __aeabi_d2uiz(double a);
extern long long __aeabi_d2lz(double a);
extern unsigned long long __aeabi_d2ulz(double a);
extern float __aeabi_d2f(double a);
extern double __aeabi_f2d(float a);
extern double __aeabi_i2d(int a);
extern double __aeabi_ui2d(unsigned int a);

typedef union
{
  double d;
  uint64_t u;
  struct
  {
    uint32_t lo;
    uint32_t hi;
  } w;
} dbl_u;

typedef union
{
  float f;
  uint32_t u;
} flt_u;

static void write_str(const char *s)
{
  while (*s)
  {
    putchar(*s++);
  }
}

static void write_hex32(uint32_t v)
{
  const char *hex = "0123456789ABCDEF";
  for (int i = 7; i >= 0; --i)
  {
    putchar(hex[(v >> (i * 4)) & 0xF]);
  }
}

static void write_hex64(uint64_t v)
{
  write_hex32((uint32_t)(v >> 32));
  write_hex32((uint32_t)v);
}

static int fail_u64(const char *name, uint64_t got, uint64_t exp)
{
  if (got == exp)
  {
    return 0;
  }
  write_str("FAIL ");
  write_str(name);
  write_str(" got=0x");
  write_hex64(got);
  write_str(" exp=0x");
  write_hex64(exp);
  write_str("\n");
  exit(1);
}

static int fail_u32(const char *name, uint32_t got, uint32_t exp)
{
  if (got == exp)
  {
    return 0;
  }
  write_str("FAIL ");
  write_str(name);
  write_str(" got=0x");
  write_hex32(got);
  write_str(" exp=0x");
  write_hex32(exp);
  write_str("\n");
  exit(1);
}

static int fail_i32(const char *name, int got, int exp)
{
  return fail_u32(name, (uint32_t)got, (uint32_t)exp);
}

static int fail_i64(const char *name, long long got, long long exp)
{
  return fail_u64(name, (uint64_t)got, (uint64_t)exp);
}

int main(void)
{
  dbl_u a, b, out;

  a.u = 0x3ff8000000000000ULL; /* 1.5 */
  b.u = 0x4000000000000000ULL; /* 2.0 */

  out.d = __aeabi_dadd(a.d, b.d);
  if (fail_u64("dadd", out.u, 0x400c000000000000ULL))
    return 1; /* 3.5 */

  out.d = __aeabi_dsub(a.d, b.d);
  if (fail_u64("dsub", out.u, 0xbfe0000000000000ULL))
    return 1; /* -0.5 */

  out.d = __aeabi_dmul(a.d, b.d);
  if (fail_u64("dmul", out.u, 0x4008000000000000ULL))
    return 1; /* 3.0 */

  /* Additional multiplication tests */
  /* 1.0 * 10.0 = 10.0 */
  a.u = 0x3FF0000000000000ULL; /* 1.0 */
  b.u = 0x4024000000000000ULL; /* 10.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_1_10", out.u, 0x4024000000000000ULL); /* 10.0 */

  /* 2.0 * 3.0 = 6.0 */
  a.u = 0x4000000000000000ULL; /* 2.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_2_3", out.u, 0x4018000000000000ULL); /* 6.0 */

  /* 3.0 * 3.0 = 9.0 */
  a.u = 0x4008000000000000ULL; /* 3.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_3_3", out.u, 0x4022000000000000ULL); /* 9.0 */

  /* 2.0 * 2.0 = 4.0 */
  a.u = 0x4000000000000000ULL; /* 2.0 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_2_2", out.u, 0x4010000000000000ULL); /* 4.0 */

  /* 0.5 * 2.0 = 1.0 */
  a.u = 0x3FE0000000000000ULL; /* 0.5 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_0p5_2", out.u, 0x3FF0000000000000ULL); /* 1.0 */

  /* 1.5 * 1.5 = 2.25 */
  a.u = 0x3FF8000000000000ULL; /* 1.5 */
  b.u = 0x3FF8000000000000ULL; /* 1.5 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_1p5_1p5", out.u, 0x4002000000000000ULL); /* 2.25 */

  /* 10.0 * 10.0 = 100.0 */
  a.u = 0x4024000000000000ULL; /* 10.0 */
  b.u = 0x4024000000000000ULL; /* 10.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_10_10", out.u, 0x4059000000000000ULL); /* 100.0 */

  /* -2.0 * 3.0 = -6.0 */
  a.u = 0xC000000000000000ULL; /* -2.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_neg2_3", out.u, 0xC018000000000000ULL); /* -6.0 */

  /* -2.0 * -3.0 = 6.0 */
  a.u = 0xC000000000000000ULL; /* -2.0 */
  b.u = 0xC008000000000000ULL; /* -3.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_neg2_neg3", out.u, 0x4018000000000000ULL); /* 6.0 */

  /* 1.0 * 0.0 = 0.0 */
  a.u = 0x3FF0000000000000ULL; /* 1.0 */
  b.u = 0x0000000000000000ULL; /* 0.0 */
  out.d = __aeabi_dmul(a.d, b.d);
  fail_u64("dmul_1_0", out.u, 0x0000000000000000ULL); /* 0.0 */

  /* Restore original test values */
  a.u = 0x3ff8000000000000ULL; /* 1.5 */
  b.u = 0x4000000000000000ULL; /* 2.0 */

  out.d = __aeabi_ddiv(a.d, b.d);
  if (fail_u64("ddiv", out.u, 0x3fe8000000000000ULL))
    return 1; /* 0.75 */

  out.d = __aeabi_dneg(a.d);
  if (fail_u64("dneg", out.u, 0xbff8000000000000ULL))
    return 1; /* -1.5 */

  if (fail_i32("dcmpeq0", __aeabi_dcmpeq(a.d, b.d), 0))
    return 1;
  if (fail_i32("dcmplt1", __aeabi_dcmplt(a.d, b.d), 1))
    return 1;
  if (fail_i32("dcmple1", __aeabi_dcmple(a.d, b.d), 1))
    return 1;
  if (fail_i32("dcmpgt0", __aeabi_dcmpgt(a.d, b.d), 0))
    return 1;
  if (fail_i32("dcmpge0", __aeabi_dcmpge(a.d, b.d), 0))
    return 1;

  if (fail_i32("dcmpeq1", __aeabi_dcmpeq(b.d, b.d), 1))
    return 1;
  if (fail_i32("dcmplt0", __aeabi_dcmplt(b.d, b.d), 0))
    return 1;
  if (fail_i32("dcmple1b", __aeabi_dcmple(b.d, b.d), 1))
    return 1;
  if (fail_i32("dcmpgt0b", __aeabi_dcmpgt(b.d, b.d), 0))
    return 1;
  if (fail_i32("dcmpge1", __aeabi_dcmpge(b.d, b.d), 1))
    return 1;

  a.u = 0x7ff8000000000001ULL; /* NaN */
  b.u = 0x3ff0000000000000ULL; /* 1.0 */
  if (fail_i32("dcmpun1", __aeabi_dcmpun(a.d, b.d), 1))
    return 1;
  if (fail_i32("dcmpun0", __aeabi_dcmpun(b.d, b.d), 0))
    return 1;

  a.u = 0x400a000000000000ULL; /* 3.25 */
  if (fail_i32("d2iz", __aeabi_d2iz(a.d), 3))
    return 1;

  a.u = 0x4016000000000000ULL; /* 5.5 */
  if (fail_u32("d2uiz", __aeabi_d2uiz(a.d), 5U))
    return 1;

  a.d = -123456789.0;
  if (fail_i64("d2lz", __aeabi_d2lz(a.d), -123456789LL))
    return 1;

  a.d = 4294967296.0; /* 2^32 */
  if (fail_u64("d2ulz", __aeabi_d2ulz(a.d), 4294967296ULL))
    return 1;

  a.d = 1.0;
  flt_u fout;
  fout.f = __aeabi_d2f(a.d);
  if (fail_u32("d2f", fout.u, 0x3f800000U))
    return 1;

  flt_u fin;
  fin.u = 0x40200000U; /* 2.5f */
  out.d = __aeabi_f2d(fin.f);
  if (fail_u64("f2d", out.u, 0x4004000000000000ULL))
    return 1; /* 2.5 */

  out.d = __aeabi_i2d(-42);
  if (fail_u64("i2d", out.u, 0xc045000000000000ULL))
    return 1; /* -42.0 */

  out.d = __aeabi_ui2d(42U);
  if (fail_u64("ui2d", out.u, 0x4045000000000000ULL))
    return 1; /* 42.0 */

  /* Additional division tests to find the bug */

  /* 4.0 / 2.0 = 2.0 (simple, powers of 2) */
  a.u = 0x4010000000000000ULL; /* 4.0 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_4_2", out.u, 0x4000000000000000ULL); /* 2.0 */

  /* 6.0 / 2.0 = 3.0 */
  a.u = 0x4018000000000000ULL; /* 6.0 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_6_2", out.u, 0x4008000000000000ULL); /* 3.0 */

  /* 6.0 / 3.0 = 2.0 */
  a.u = 0x4018000000000000ULL; /* 6.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_6_3", out.u, 0x4000000000000000ULL); /* 2.0 */

  /* 9.0 / 3.0 = 3.0 */
  a.u = 0x4022000000000000ULL; /* 9.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_9_3", out.u, 0x4008000000000000ULL); /* 3.0 */

  /* 7.0 / 2.0 = 3.5 (non-integer result with power of 2 divisor) */
  a.u = 0x401C000000000000ULL; /* 7.0 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_7_2", out.u, 0x400C000000000000ULL); /* 3.5 */

  /* 5.0 / 2.0 = 2.5 */
  a.u = 0x4014000000000000ULL; /* 5.0 */
  b.u = 0x4000000000000000ULL; /* 2.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_5_2", out.u, 0x4004000000000000ULL); /* 2.5 */

  /* 1.0 / 3.0 = 0.333... (repeating decimal) */
  a.u = 0x3FF0000000000000ULL; /* 1.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_1_3", out.u, 0x3FD5555555555555ULL); /* 0.333... */

  /* 2.0 / 3.0 = 0.666... */
  a.u = 0x4000000000000000ULL; /* 2.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_2_3", out.u, 0x3FE5555555555555ULL); /* 0.666... */

  /* Test 10.0 / 3.0 = 3.333... (reproduces division bug) */
  /* 10.0 = 0x4024000000000000, 3.0 = 0x4008000000000000 */
  /* 10/3 = 3.333... = 0x400AAAAAAAAAAAAB (rounded) */
  a.u = 0x4024000000000000ULL; /* 10.0 */
  b.u = 0x4008000000000000ULL; /* 3.0 */
  out.d = __aeabi_ddiv(a.d, b.d);
  fail_u64("ddiv_10_3", out.u, 0x400AAAAAAAAAAAABULL); /* 3.333... (rounded) */

  write_str("PASS\n");
  return 0;
}
