/* A function that only hands its parameters, untouched, to a callee of the
 * same signature becomes a branch to it (pure forward): register, split,
 * stack and home-slot struct arguments, 64-bit and double scalars.  The
 * near-misses must stay ordinary calls: swapped arguments, a modified
 * parameter, a narrower parameter type, a struct return. */
#include <stdio.h>

struct T { int a[11]; };
struct S { int x, y; };

__attribute__((noinline)) static int g1(int k, struct T t)
{
  int r = k;
  for (int i = 0; i < 11; i++)
    r = r * 3 + t.a[i];
  return r;
}
__attribute__((noinline)) static int f1(int k, struct T t) { return g1(k, t); }

__attribute__((noinline)) static int g2(struct S s) { return s.x * 100 + s.y; }
__attribute__((noinline)) static int f2(struct S s) { return g2(s); }

__attribute__((noinline)) static double g3(int a, long long b, double c, int d, int e)
{
  return a + (double)b * 2 + c * 3 + d * 5 + e * 7;
}
__attribute__((noinline)) static double f3(int a, long long b, double c, int d, int e) { return g3(a, b, c, d, e); }

static int sink;
__attribute__((noinline)) static void g4(int a, int b, int c, int d, struct T t, struct S s)
{
  sink = a + b * 2 + c * 3 + d * 4 + t.a[10] * 5 + s.y * 6;
}
__attribute__((noinline)) static void f4(int a, int b, int c, int d, struct T t, struct S s) { g4(a, b, c, d, t, s); }

/* Near-misses. */
__attribute__((noinline)) static int g5(int a, int b) { return a * 10 + b; }
__attribute__((noinline)) static int swapped(int a, int b) { return g5(b, a); }
__attribute__((noinline)) static int modified(int a, int b)
{
  a += 1;
  return g5(a, b);
}
__attribute__((noinline)) static int g6(short a) { return a * 2; }
__attribute__((noinline)) static int narrower(int a) { return g6((short)a); }
__attribute__((noinline)) static struct S g7(struct S s) { return (struct S){s.y, s.x}; }
__attribute__((noinline)) static struct S sret(struct S s) { return g7(s); }

int main(void)
{
  struct T t;
  for (int i = 0; i < 11; i++)
    t.a[i] = i * i - 5;
  struct S s = {7, 9};
  printf("%d %d\n", f1(3, t), f2(s));
  printf("%d\n", (int)f3(1, 1000000000000LL / 1000000, 2.5, 4, 5));
  f4(1, 2, 3, 4, t, s);
  printf("%d\n", sink);
  printf("%d %d %d\n", swapped(1, 2), modified(1, 2), narrower(70000));
  struct S r = sret(s);
  printf("%d %d\n", r.x, r.y);
  return 0;
}
