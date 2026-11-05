/* -O0 does not walk a function twice to size its scratch save area: it reserves
 * a fixed one, and an instruction wanting more PUSHes around itself.  Everything
 * here needs scratch registers while the argument and callee-saved registers are
 * full, in frames with SP-relative slots above and below those saves. */
#include <stdio.h>

typedef unsigned long long u64;

__attribute__((noinline)) static u64 mix(u64 a, u64 b, int c, int d, u64 e, int f)
{
  return (a ^ (b << 3)) + (u64)c * 7 + (u64)d * 11 + (e >> 2) + (u64)f;
}

/* 64-bit values live across calls, stack arguments, nested calls. */
__attribute__((noinline)) static u64 pressure(u64 a, u64 b, u64 c, int d, int e, int f, int g, int h)
{
  u64 x = a * b + c;
  u64 y = (x >> 7) ^ (a << 9);
  u64 z = mix(x, y, d, e, mix(y, c, f, g, x, h), mix(a, b, g, h, c, d) & 0xff);
  int i, s = 0;
  for (i = 0; i < 5; i++)
    s += (int)((x >> i) & 3) + d * e - f + g * h;
  return x + y + z + (u64)s + a / (b | 1) + c % 13;
}

/* A frame past the 12-bit STR offset: the save slot may be out of range. */
__attribute__((noinline)) static int big_frame(int n, u64 v, int p, int q, int r)
{
  volatile char pad[6000];
  int small[8];
  int i;
  for (i = 0; i < 8; i++)
    small[i] = n * i + p - q + r;
  pad[0] = (char)n;
  pad[5999] = (char)(n + 1);
  v = v * 3 + small[n & 7];
  return (int)(v >> 2) + pad[0] + pad[5999] + small[7];
}

/* Struct copies and division around a call with stack arguments. */
struct blob { int a[9]; u64 w; };

__attribute__((noinline)) static struct blob make(int s, int t, int u, int v, int w, int x)
{
  struct blob b;
  int i;
  for (i = 0; i < 9; i++)
    b.a[i] = s + t * i - u / (v | 1) + w % (x | 1);
  b.w = (u64)s << 33 | (unsigned)t;
  return b;
}

__attribute__((noinline)) static int use_blob(struct blob b, int k, int l, int m, int n, int o)
{
  struct blob c = b;
  c.a[k & 7] += l - m + n / (o | 1);
  return c.a[0] + c.a[k & 7] + c.a[8] + (int)(c.w >> 33) + (int)c.w;
}

int main(void)
{
  struct blob b = make(3, 5, 77, 9, 100, 7);
  printf("%llx\n", mix(1, 2, 3, 4, 5, 6));
  printf("%llx\n", pressure(0x1234567890abULL, 0xfedcba987ULL, 42, 1, 2, 3, 4, 5));
  printf("%d\n", big_frame(9, 0x123456789ULL, 11, 22, 33));
  printf("%d\n", use_blob(b, 3, 7, 8, 90, 6));
  printf("%d\n", use_blob(make(1, 2, 3, 4, 5, 6), mix(1, 2, 3, 4, 5, 6) & 7, 1, 2, 3, 4));
  return 0;
}
