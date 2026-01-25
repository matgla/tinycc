#include <stdint.h>
#include <stdlib.h>
extern int putchar(int c);

extern unsigned int __aeabi_d2uiz(double a);

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

int main(void)
{
  dbl_u a, b, out;

  a.u = 0x4016000000000000ULL; /* 5.5 */
  if (fail_u32("d2uiz", __aeabi_d2uiz(a.d), 5U))
    return 1;
  return 0;
}
