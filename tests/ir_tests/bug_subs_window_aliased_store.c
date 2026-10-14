/* ADD/SUB -> ADDS/SUBS fusion across a store window: the store may write
 * the stack slot of the tested value (address-taken local), so the CMP that
 * re-reads it must not be replaced by the SUBS flags.  At -O0 h1(1) returned
 * 100 instead of 5. */
#include <stdio.h>

int *gp;
__attribute__((noinline)) void leak(int *p) { gp = p; }

__attribute__((noinline)) int h1(int x)
{
  int v = x;
  int *p = &v;
  leak(p);
  v = v - 1;
  *p = 5;
  if (v == 0)
    return 100;
  return v;
}

__attribute__((noinline)) int h2(int x)
{
  int v = x;
  leak(&v);
  v = v - 1;
  *gp = 0;
  if (v != 0)
    return v;
  return 200;
}

__attribute__((noinline)) int h3(int x)
{
  int v = x;
  leak(&v);
  v = v - 1;
  *gp = 7;
  int r = (v == 0) ? 300 : v;
  return r;
}

__attribute__((noinline)) int h4(int x)
{
  int v[2];
  v[0] = x;
  leak(&v[0]);
  v[0] = v[0] - 1;
  *gp = 0;
  if (v[0] == 0)
    return 400;
  return v[0];
}

int main(void)
{
  printf("%d %d\n", h1(1), h1(3));
  printf("%d %d\n", h2(1), h2(3));
  printf("%d %d\n", h3(1), h3(3));
  printf("%d %d\n", h4(1), h4(3));
  return 0;
}
