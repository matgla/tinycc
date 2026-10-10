/* ub_only_body_elide emptied a nested function whose only effect was a store
 * through a captured pointer: its StackLoc reads are the parent's slots via
 * the static chain, and were taken for reads of uninitialised locals of its
 * own (UB), so the whole body was "UB only".  -O2/-Os.  Found by the hazard
 * sandwich tests (2026-10-06). */
#include <stdio.h>

__attribute__((noinline)) int s_load(int *p, int v)
{
  void setit(void) { *p = v; }
  int a = *p;
  setit();
  int b = *p;
  return a * 1000 + b;
}

__attribute__((noinline)) int s_local(int v)
{
  int x = 1;
  int *p = &x;
  void setit(void) { *p = v; }
  int a = x;
  setit();
  return a * 1000 + x;
}

int G;

int main(void)
{
  G = 1;
  printf("%d %d\n", s_load(&G, 5), G);
  printf("%d\n", s_local(6));
  return 0;
}
