#include <stdio.h>

/* entry_store_prop forwards a local's initial values into later loads unless
 * the local escapes.  An address reaching a call through a VAR or a copy
 * chain escaped only the word it named, so the call's writes to the others
 * were lost. */
typedef struct { int a, b, c; } T;
__attribute__((noinline)) void wr(T *d) { d->a = 11; d->b = 12; d->c = 13; }
__attribute__((noinline)) void wri(int *d) { d[0] = 21; d[1] = 22; }
T *stash;
__attribute__((noinline)) void wstash(void) { stash->c = 33; }

__attribute__((noinline)) int f(void)
{
  T x = {1, 2, 3};
  T *p = &x;
  wr(p);
  printf("x %d %d %d %d\n", x.a, x.b, x.c, p->a);
  return 0;
}

__attribute__((noinline)) int f_add(void)
{
  T x = {1, 2, 3};
  T *p = &x;
  int *q = &p->b;
  wri(q);
  printf("add %d %d %d\n", x.a, x.b, x.c);
  return 0;
}

__attribute__((noinline)) int f_global(void)
{
  T x = {1, 2, 3};
  T *p = &x;
  stash = p;
  wstash();
  printf("glob %d %d %d\n", x.a, x.b, x.c);
  return 0;
}

typedef struct { int a, b, c, d, e, f; } S;
static S f1(S *p) { S r; r.a = 100; r.b = p->a + 1; r.c = p->b + 2; r.d = p->c; r.e = p->d; r.f = p->e; return r; }

int main(void)
{
  f();
  f_add();
  f_global();
  S s = {5, 6, 7, 8, 9, 10};
  S *ps = &s;
  *ps = f1(ps);
  printf("%d %d %d %d %d %d\n", s.a, s.b, s.c, s.d, s.e, s.f);
  return 0;
}
