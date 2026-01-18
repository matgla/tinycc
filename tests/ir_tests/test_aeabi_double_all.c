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
  return 1;
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
  return 1;
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

  write_str("PASS\n");
  return 0;
}
