/* Stack-passed parameters read more than once stay in the register the
 * allocator gave them, loaded by the prologue: 32- and 64-bit ones, near and
 * far (a frame larger than the 12-bit immediate), off SP and off the frame
 * pointer, rewritten in a loop, live across calls, and in variadic
 * functions.  A by-value struct's parameter names its memory and stays there,
 * whether split across r3 and the stack or wholly on the stack. */
#include <stdarg.h>
#include <stdio.h>

__attribute__((noinline)) static int ident(int x)
{
  return x;
}

__attribute__((noinline)) static int six(int a, int b, int c, int d, int e, int f)
{
  return (a - e) * (b + f) + e * f - (c ^ e) + (d | f);
}

__attribute__((noinline)) static long long wide(int a, int b, int c, long long x, long long y)
{
  return x * y + (x >> 3) - y * 4 + a + b + c + (x ^ y);
}

__attribute__((noinline)) static double dbl(int a, int b, int c, int d, double x, double y)
{
  return x * y + x / 4.0 - y + a + b + c + d;
}

__attribute__((noinline)) static int far_frame(int a, int b, int c, int d, int e, long long w, int f)
{
  volatile char buf[6000];
  for (int i = 0; i < (int)sizeof(buf); i += 97)
    buf[i] = (char)((i + e) & 0x3f);
  int s = buf[97 * e] + buf[97 * f];
  return s + e * f + (int)(w >> 8) + (int)w + e - f + a + b + c + d;
}

__attribute__((noinline)) static int rewritten(int a, int b, int c, int d, int e, int n)
{
  for (int i = 0; i < n; i++)
  {
    e = e * 3 + i;
    if (e > 1000)
      e %= 997;
  }
  return e + n + a + b + c + d;
}

__attribute__((noinline)) static int across_calls(int a, int b, int c, int d, int e, int f)
{
  int s = ident(e) + ident(f);
  s += ident(a + e) * f;
  return s + e - f + b + c + d;
}

__attribute__((noinline)) static int with_vla(int a, int b, int c, int d, int e, int f)
{
  int v[e + 1];
  for (int i = 0; i <= e; i++)
    v[i] = i * f;
  return v[e] + v[e / 2] + e + f + a + b + c + d;
}

__attribute__((noinline)) static int varsum(int a, int b, int c, int d, int e, int n, ...)
{
  va_list ap;
  int s = e * n + e + n;
  va_start(ap, n);
  for (int i = 0; i < n; i++)
    s += va_arg(ap, int) * e;
  va_end(ap);
  return s + a + b + c + d;
}

__attribute__((noinline)) static int narrow(int a, int b, int c, int d, signed char e, unsigned short f)
{
  return e * f + e - f + (e < 0) + a + b + c + d;
}

struct pt
{
  int v[6];
};

__attribute__((noinline)) static int split_struct(int a, int b, int c, struct pt p, int e)
{
  int s = e;
  for (int i = 0; i < 6; i++)
    s = s * 3 + p.v[i];
  return s + p.v[e % 6] + a + b + c;
}

/* The Zig C backend's shape: the local copy becomes the parameter's own memory
 * (param_copy_alias), read through a pointer and never address-taken. */
__attribute__((noinline)) static int stack_struct(int a, int b, int c, int d, struct pt p, int e)
{
  struct pt t0;
  struct pt const *t1;
  t0 = p;
  t1 = (struct pt const *)&t0;
  return t1->v[1] * e + t1->v[4] - t1->v[0] + a + b + c + d;
}

int main(void)
{
  struct pt p = {{3, -4, 5, 11, -2, 7}};
  printf("%d\n", six(1, 2, 3, 4, 5, 6));
  printf("%lld\n", wide(1, 2, 3, 123456789012LL, -98765LL));
  printf("%d\n", (int)dbl(1, 2, 3, 4, 2.5, -8.0));
  printf("%d\n", far_frame(1, 2, 3, 4, 5, 0x123456789LL, 7));
  printf("%d\n", rewritten(1, 2, 3, 4, 5, 20));
  printf("%d\n", across_calls(1, 2, 3, 4, 5, 6));
  printf("%d\n", with_vla(1, 2, 3, 4, 9, 3));
  printf("%d\n", varsum(1, 2, 3, 4, 5, 3, 10, 20, 30));
  printf("%d\n", narrow(1, 2, 3, 4, -7, 40000));
  printf("%d\n", split_struct(1, 2, 3, p, 6));
  printf("%d\n", stack_struct(1, 2, 3, 4, p, 6));
  return 0;
}
