/* Struct values more than 32 KiB from the frame base.  A struct-typed stack
 * operand keeps its offset in a 16-bit field, so these are named through
 * their address instead: a small struct passed in registers, one split across
 * r3 and the stack, one passed wholly on the stack, a struct returned into a
 * far local, and a by-value parameter that sits behind a big one. */
#include <stdio.h>
#include <string.h>

struct S { int a, b, c; };
struct L { int a[8]; };
struct Big { char pad[40000]; int tag; };

__attribute__((noinline)) static int f(struct S s) { return s.a + s.b * 3 + s.c * 5; }
__attribute__((noinline)) static int g(struct L l)
{
  int r = 0;
  for (int i = 0; i < 8; i++)
    r += l.a[i] * (i + 1);
  return r;
}
__attribute__((noinline)) static int on_stack(int x, int y, int z, int w, struct S s) { return x + y + z + w + f(s); }
__attribute__((noinline)) static struct S mk(int k) { return (struct S){k, k * 2, k * 3}; }

__attribute__((noinline)) static int far_locals(int k)
{
  volatile char pad[40000];
  struct S s = {k, k + 1, k + 2};
  struct L l;
  for (int i = 0; i < 8; i++)
    l.a[i] = k + i;
  struct S t = mk(k + 10);
  pad[0] = (char)k;
  pad[39999] = pad[0];
  return f(s) + g(l) + on_stack(1, 2, 3, 4, s) + f(t) + pad[39999];
}

__attribute__((noinline)) static int behind_big(struct Big b, struct S s) { return b.tag + f(s) + on_stack(0, 0, 0, b.tag, s); }

static struct Big big;

int main(void)
{
  printf("%d %d\n", far_locals(1), far_locals(100));
  big.tag = 7;
  printf("%d\n", behind_big(big, (struct S){2, 3, 4}));
  return 0;
}
