/* A K&R definition's caller passes the promoted argument (int for a narrow
 * integer); the callee converts it to the parameter's
 * declared type.  An inlined expansion -- full (always_inline) or a guard
 * head's replayed test (docs/inline_guard_heads.md) -- bound the promoted
 * value to the parameter without that conversion: `fa(t)` with t = 200 tested
 * 200 < 0 rather than (signed char)200 < 0 and skipped the work. */
#include <stdio.h>

unsigned hits;
__attribute__((noinline)) void rec(int v) { hits = hits * 3 + v; }
#define WORK(v)                                                                                                        \
  rec(v);                                                                                                              \
  rec(1);                                                                                                              \
  rec(2);                                                                                                              \
  rec(3);                                                                                                              \
  rec(4);                                                                                                              \
  rec(5);                                                                                                              \
  rec(6);                                                                                                              \
  rec(7);                                                                                                              \
  rec(8)

/* Fully inlined. */
static __attribute__((always_inline)) inline void fa(c) signed char c;
{
  if (c < 0)
    hits += 1000;
}
static __attribute__((always_inline)) inline void fu(c) unsigned char c;
{
  if (c == 4)
    hits += 2000;
}
static __attribute__((always_inline)) inline void fs(s) short s;
{
  if (s < 0)
    hits += 4000;
}

/* Out of line: a guard head's test replayed at the call site at -O2. */
static void gk(c) signed char c;
{
  if (c < 0)
  {
    WORK(c & 15);
  }
}
static void gu(c) unsigned char c;
{
  if (c == 4)
  {
    WORK(c);
  }
}
static void gs(s) unsigned short s;
{
  if (s == 7)
  {
    WORK(s);
  }
}

__attribute__((noinline)) void a(int n)
{
  int t = n + 199;
  fa(t);
  fu(t + 60);
  fs(t + 65335);
}
__attribute__((noinline)) void k3(int n)
{
  int t = n + 199;
  gk(t);
}
__attribute__((noinline)) void k4(int *x)
{
  int t = *x;
  gk(t);
  gu(t + 60);
  gs(t + 65343);
}

int main(void)
{
  int v = 200;
  a(1);
  printf("A %u\n", hits);
  hits = 0;
  k3(1);
  printf("K %u\n", hits);
  k4(&v);
  printf("X %u\n", hits);
  return 0;
}
