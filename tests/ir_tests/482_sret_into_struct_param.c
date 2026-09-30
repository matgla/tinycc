/* `s = f(s)` where `s` is a by-value struct parameter that arrived (at least
   partly) in the caller's argument area.  The call's sret pointer is `&s`, and
   the argument move for it read the slot instead of computing its address, so
   f wrote its result through the struct's first word -- gcc_execute/20040703-1
   exited 255 on the device at every -O level.

   Three placements: split across r1-r3 and the stack behind an sret pointer
   (the torture test's num_rshift/num_lshift), split behind an int, and wholly
   on the stack behind four ints. */
#include <stdio.h>

struct S
{
  unsigned a, b;
  int c, d;
};

__attribute__((noinline)) static struct S bump(struct S s, unsigned k)
{
  s.a += k;
  s.b += 2 * k;
  s.c += 3 * (int)k;
  s.d += 4 * (int)k;
  return s;
}

__attribute__((noinline)) static struct S behind_sret(struct S s, unsigned k)
{
  s = bump(s, k);
  s.d = -s.d;
  return s;
}

__attribute__((noinline)) static int behind_int(int x, struct S s)
{
  s = bump(s, (unsigned)x);
  return (int)(s.a + s.b) + s.c + s.d;
}

__attribute__((noinline)) static int on_stack(int p, int q, int r, int t, struct S s)
{
  s = bump(s, (unsigned)(p + q + r + t));
  return (int)(s.a * 1000 + s.b * 100) + s.c * 10 + s.d;
}

int main(void)
{
  struct S s = {1, 2, 3, 4};
  struct S r = behind_sret(s, 10);
  printf("%u %u %d %d\n", r.a, r.b, r.c, r.d);
  printf("%d\n", behind_int(5, s));
  printf("%d\n", on_stack(1, 1, 1, 1, s));
  return 0;
}
