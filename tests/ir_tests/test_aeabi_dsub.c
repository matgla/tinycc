#include <stdint.h>

extern int putchar(int c);
extern double __aeabi_dsub(double a, double b);

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

int main(void)
{
  dbl_u a, b, out;

  a.u = 0x3ff8000000000000ULL; /* 1.5 */
  b.u = 0x4000000000000000ULL; /* 2.0 */

  out.d = __aeabi_dsub(a.d, b.d);
  if (fail_u64("dsub", out.u, 0xbfe0000000000000ULL))
    return 1; /* -0.5 */

  return 0;
}
