/* ssa:load_cse: a global store of one type must kill a tracked same-size store
 * of another type to the same bytes (union float/int and double/long long pun)
 * (docs/bugs/ssa-load-cse-gstore-same-size-btype-pun-stale, fixed). */
#include <stdio.h>

union U { float f; int i; } u;
union D { double d; long long l; } ud;

__attribute__((noinline)) float t1(float x)
{
  u.f = x;
  u.i = 0x40000000;
  return u.f;
}

__attribute__((noinline)) double t2(double x)
{
  ud.d = x;
  ud.l = 0x4000000000000000LL;
  return ud.d;
}

__attribute__((noinline)) int t3(int v)
{
  u.i = v;
  u.f = 1.0f;
  return u.i;
}

int main(void)
{
  printf("%d %d\n", (int)t1(1.0f), (int)t2(1.0));
  printf("%x\n", t3(5));
  return 0;
}
