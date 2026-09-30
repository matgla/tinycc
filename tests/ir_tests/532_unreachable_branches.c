/* __builtin_unreachable() marks where control never goes, and the branches
   that could only lead there are folded: an exhaustive switch loses its
   range check and last compare, a loop whose exit is unreachable its bound.
   Every reachable path must still compute what it did.  The Zig C backend
   ends every exhaustive switch with `default: zig_unreachable();`. */
#include <stdio.h>

enum tag { T_A, T_B, T_C, T_D, T_E, T_F };

__attribute__((noinline)) const char *tag_name(enum tag t)
{
  switch (t)
  {
  case T_A: return "alpha";
  case T_B: return "beta";
  case T_C: return "gamma";
  case T_D: return "delta";
  case T_E: return "epsilon";
  case T_F: return "zeta";
  }
  __builtin_unreachable();
}

__attribute__((noinline)) int pick(int t, int x)
{
  switch (t)
  {
  case 0: return x + 1;
  case 1: return x * 3;
  case 2: return x - 7;
  default: __builtin_unreachable();
  }
}

/* ULEB128 of a u16: at most three bytes, the third iteration's exit is unreachable. */
__attribute__((noinline)) int uleb(unsigned v, unsigned char *out)
{
  for (unsigned i = 0;; i++)
  {
    if (i < 3)
    {
      unsigned char b = v & 127;
      v >>= 7;
      if (v == 0)
      {
        out[i] = b;
        return (int)i + 1;
      }
      out[i] = b | 128;
      continue;
    }
    __builtin_unreachable();
  }
}

/* A value computed on the doomed side must not leak into the live one. */
__attribute__((noinline)) int side(int k)
{
  int r = k * 5;
  if (k > 100)
  {
    r = 99;
    __builtin_unreachable();
  }
  return r + 1;
}

int main(void)
{
  unsigned total = 0;
  for (int t = 0; t < 6; t++)
  {
    printf("%d %s\n", t, tag_name((enum tag)t));
    total += (unsigned)tag_name((enum tag)t)[1];
  }
  for (int t = 0; t < 3; t++)
  {
    int r = pick(t, 40 + t);
    printf("pick %d %d\n", t, r);
    total += (unsigned)r;
  }
  unsigned vals[] = {0, 1, 127, 128, 300, 16383, 16384, 65535};
  for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++)
  {
    unsigned char buf[4] = {0};
    int n = uleb(vals[i], buf);
    printf("uleb %u -> %d: %02x %02x %02x\n", vals[i], n, buf[0], buf[1], buf[2]);
    total += (unsigned)n * 1000u + buf[0] + buf[1] * 7u + buf[2] * 13u;
  }
  for (int k = 0; k < 5; k++)
    total += (unsigned)side(k);
  printf("total=%u\n", total);
  return 0;
}
