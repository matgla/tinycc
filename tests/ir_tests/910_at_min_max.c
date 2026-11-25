/* @min / @max: each operand evaluated once, left to right; the usual
 * arithmetic type; integers as cmp/it/mov, float as vminnm/vmaxnm.f32 (FPv5)
 * or fminf/fmaxf, double as fmin/fmax; a NaN loses to a number; constants
 * fold (static initializers, sizeof). */
#include <stdio.h>

typedef unsigned int size_t_;
#define PATH_MAX 4096

static int calls;
static int seq(int v)
{
  calls = calls * 10 + v;
  return v;
}

static size_t_ cap(size_t_ buflen) { return @min(buflen, PATH_MAX); }
static int imin(int a, int b) { return @min(a, b); }
static int imax(int a, int b) { return @max(a, b); }
static unsigned umax(unsigned a, unsigned short s) { return @max(a, s); }
static int smin(unsigned short s, int i) { return @min(s, i); } /* s promotes to int */
static long long llmin(long long a, long long b) { return @min(a, b); }
static unsigned long long ullmax(unsigned long long a, unsigned long long b) { return @max(a, b); }
static long long mixed(int a, long long b) { return @max(a, b); }
static int postinc(int x) { return @min(x, x++) + x * 100; } /* the first operand is x before x++ */
static float fmn(float a, float b) { return @min(a, b); }
static float fmx(float a, float b) { return @max(a, b); }
static double dmn(double a, double b) { return @min(a, b); }
static double dmx(double a, double b) { return @max(a, b); }
static int clamp(int v, int lo, int hi) { return @max(lo, @min(v, hi)); }

static const int folded = @max(3, -7) + @min(10u, 20u);
static const float ffolded = @min(1.5f, -2.5f);

int main(void)
{
  volatile int zero = 0;
  float nanf_ = 0.0f / (float)zero;
  double nand_ = 0.0 / (double)zero;

  printf("cap %u %u %u\n", cap(17), cap(4096), cap(100000));
  printf("imin %d %d %d\n", imin(3, 5), imin(-3, 5), imin(-2147483647 - 1, 2147483647));
  printf("imax %d %d %d\n", imax(3, 5), imax(-3, -5), imax(-2147483647 - 1, 2147483647));
  printf("umax %u %u\n", umax(7, 65535), umax(4000000000u, 9));
  printf("smin %d %d\n", smin(65535, -1), smin(3, 9));
  printf("llmin %lld %lld\n", llmin(-5000000000LL, 1), llmin(5000000000LL, 4999999999LL));
  printf("ullmax %llu\n", ullmax(18000000000000000000ULL, 5));
  printf("mixed %lld %lld\n", mixed(-1, -5000000000LL), mixed(7, 5000000000LL));
  calls = 0;
  int r = @min(seq(4), seq(2));
  printf("once %d calls %d\n", r, calls);
  printf("postinc %d\n", postinc(5));
  printf("clamp %d %d %d\n", clamp(-9, 0, 10), clamp(5, 0, 10), clamp(99, 0, 10));
  printf("fmin %d %d\n", (int)(fmn(1.5f, -2.25f) * 4), (int)(fmx(1.5f, -2.25f) * 4));
  printf("fnan %d %d %d %d\n", (int)fmn(nanf_, 3.0f), (int)fmn(3.0f, nanf_), (int)fmx(nanf_, -3.0f),
         (int)fmx(-3.0f, nanf_));
  printf("dmin %d %d\n", (int)(dmn(1.5, -2.25) * 4), (int)(dmx(1.5, -2.25) * 4));
  printf("dnan %d %d\n", (int)dmn(nand_, 3.0), (int)dmx(-3.0, nand_));
  printf("folded %d %d sizeof %d %d\n", folded, (int)(ffolded * 2), (int)sizeof(@min((char)1, (short)2)),
         (int)sizeof(@max(1, 2LL)));
  int acc = 0;
  for (int i = -5; i < 5; i++)
    acc += @max(i, 0) * 10 + @min(i, 0);
  printf("loop %d\n", acc);
  return 0;
}
