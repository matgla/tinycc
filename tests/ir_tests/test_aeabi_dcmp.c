#include <stdint.h>

extern int putchar(int c);

extern int __aeabi_dcmpeq(double a, double b);
extern int __aeabi_dcmplt(double a, double b);
extern int __aeabi_dcmple(double a, double b);
extern int __aeabi_dcmpgt(double a, double b);
extern int __aeabi_dcmpge(double a, double b);
extern int __aeabi_dcmpun(double a, double b);

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

int main(void)
{
  dbl_u a, b;

  a.u = 0x3ff8000000000000ULL; /* 1.5 */
  b.u = 0x4000000000000000ULL; /* 2.0 */

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

  return 0;
}
