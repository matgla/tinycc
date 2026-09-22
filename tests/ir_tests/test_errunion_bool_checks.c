/* The Zig C backend's error handling: a call returns a small struct in r0
 * (an error union `{ uint16_t error; }`), the caller copies it and tests the
 * field through one `bool` reused by every check.  The field is the stored
 * register's low bits, zero-extended -- the bits of r0 above it are
 * unspecified, and here they are garbage -- and each check branches on its
 * own compare.  A bool also read outside its test, or reached through a
 * pointer, keeps its value. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct eu16
{
  uint16_t error;
};
struct eu8
{
  uint8_t error;
};

union word16
{
  uint32_t w;
  struct eu16 s;
};
union word8
{
  uint32_t w;
  struct eu8 s;
};

static volatile uint32_t junk = 0xABCD0000u;

/* The returned register holds the struct and garbage above it. */
__attribute__((noinline)) static struct eu16 fail16(int x)
{
  union word16 u;
  u.w = junk | (uint32_t)(x % 3 == 0 ? 0 : 100 + x);
  return u.s;
}

__attribute__((noinline)) static struct eu8 fail8(int x)
{
  union word8 u;
  u.w = (junk << 4) | (uint32_t)(x % 4 == 0 ? 0 : 7 + x);
  return u.s;
}

static int seen;
__attribute__((noinline)) static void note(uint16_t e)
{
  seen += e;
}

__attribute__((noinline)) static uint16_t run(int x)
{
  struct eu16 t0;
  struct eu8 t3;
  bool t1;
  uint16_t t2;
  t0 = fail16(x);
  t1 = t0.error == UINT16_C(0);
  if (t1)
  {
    goto block_0;
  }
  t2 = t0.error;
  note(t2);
  return t2;
block_0:;
  t3 = fail8(x);
  t1 = t3.error == UINT8_C(0);
  if (t1)
  {
    goto block_1;
  }
  return (uint16_t)(t3.error + 1000);
block_1:;
  t0 = fail16(x + 1);
  t1 = t0.error != UINT16_C(0);
  if (t1)
  {
    return (uint16_t)(t0.error + 2000);
  }
  return 0;
}

/* The bool is also returned: its checks keep materialising it. */
__attribute__((noinline)) static int kept(int x)
{
  bool t1;
  t1 = fail16(x).error == 0;
  if (t1)
    x += 10;
  t1 = fail16(x + 1).error == 0;
  if (t1)
    x += 20;
  return x * 2 + t1;
}

__attribute__((noinline)) static void flip(bool *p)
{
  *p = !*p;
}

/* Reached through a pointer. */
__attribute__((noinline)) static int through_pointer(int x)
{
  bool t1;
  t1 = fail16(x).error == 0;
  flip(&t1);
  if (t1)
    return 1;
  t1 = fail8(x).error == 0;
  if (t1)
    return 2;
  return 3;
}

int main(void)
{
  unsigned sum = 0;
  for (int x = 0; x < 24; x++)
    sum = sum * 31 + run(x);
  printf("%u %d\n", sum, seen);
  unsigned k = 0;
  for (int x = 0; x < 12; x++)
    k = k * 7 + (unsigned)kept(x);
  printf("%u\n", k);
  unsigned t = 0;
  for (int x = 0; x < 12; x++)
    t = t * 5 + (unsigned)through_pointer(x);
  printf("%u\n", t);
  return 0;
}
