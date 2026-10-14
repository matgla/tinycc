/*
 *  TCC Tests - a GNU nested function's name is scoped to its block
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The name was pushed with global_identifier_push: below every block-scope
 * binding, never unbound, so it ignored shadowing and outlived its block and
 * its parent, and bodies compiled after the parent saw nested functions
 * their definition did not.  nested_define binds it in the defining block;
 * each body is compiled with the names its definition saw. */
#include <stdio.h>

#define NI __attribute__((noinline))

int g(int x) { return x + 1; }
int q(int x) { return x - 1; }
int gv = 40;

/* a block-scope prototype, then a nested g in an inner block */
NI int j1(int k)
{
  int g(int);
  NI int a(int x) { return g(x) + k; }
  int r = 0;
  {
    NI int g(int x) { return x + 1000; }
    r += g(1);
  }
  r += a(1) * 10;
  return r;
}

NI int j2(int k)
{
  int r = 0;
  {
    int g(int);
    r += g(2);
  }
  NI int a(int x) { return g(x) + k; }
  {
    NI int g(int x) { return x + 1000; }
    r += g(1) * 100;
  }
  r += a(1) * 10000;
  return r;
}

/* a static local and an enum constant shadow an outer nested q */
NI int st(int k)
{
  NI int q(int x) { return x + 1000; }
  int r = 0;
  {
    static int q = 7;
    NI int m(void) { return q * 3 + k; }
    r += m();
  }
  return r + q(1) * 100;
}

NI int en(int k)
{
  NI int q(int x) { return x + 1000; }
  int r = 0;
  {
    enum { q = 9 };
    NI int m(void) { return q * 3 + k; }
    r += m();
  }
  r += q(1) * 1000;
  return r;
}

/* a nested q defined in a block hides the parent's int q there only */
NI int lq(void)
{
  int q = 5;
  {
    NI int q(int y) { return y + 100; }
    q(1);
  }
  NI int w(void) { return q * 3; }
  return w();
}

/* a block's nested q ends with the block */
NI int ob(void)
{
  NI int q(int y) { return y + 1000; }
  int r = 0;
  {
    NI int q(int y) { return y + 1; }
    r += q(0);
  }
  NI int s(int y) { return q(y) + 2; }
  return r + s(2);
}

/* nested functions end with their parent */
NI int twice_parent(int a)
{
  NI int twice(int x) { return x * 2 + a; }
  return twice(500);
}
int twice(int x) { return x * 10; }
NI int after_parent(int x) { return twice(x) + q(3); }

/* a body compiled after its parent sees what its definition saw */
NI int ea(int k)
{
  NI int n(int y) { return q(y) + k; }
  int r = 0;
  {
    NI int q(int x) { return x + 50; }
    r = q(0) * 100 + n(20);
  }
  return r;
}

NI int ec(int k)
{
  NI int n(void) { return gv + k; }
  int r = 0;
  {
    NI int gv(int x) { return x; }
    r = gv(3) * 100 + n();
  }
  return r;
}

/* `auto` forward declarations bind the later definition */
NI int fw(int k)
{
  auto int odd(int);
  NI int even(int n) { return n ? odd(n - 1) : 1 + k; }
  NI int odd(int n) { return n ? even(n - 1) : 0 + k; }
  return even(10) * 10 + odd(7);
}

/* a parameter of an enclosing nested function hides an outer nested q */
NI int ps(int k)
{
  NI int q(int x) { return x + 1000; }
  NI int p(int q)
  {
    NI int n(void) { return q * 2 + k; }
    return n();
  }
  return p(5) + q(1) * 100;
}

/* a parent's static local, read and written by a nested function */
NI int sl(void)
{
  static int z = 5;
  NI int m(void) { return z++; }
  return m() + m() * 10 + z * 100;
}

/* typedefs and enum constants of one block do not reach a sibling defined
 * after it (compiled while the first body is being finished) */
typedef int T;
enum { K = 3 };
NI int sib(void)
{
  int r = 0;
  {
    typedef char T;
    enum { K = 100 };
    NI int s1(void) { return (int)sizeof(T) + K; }
    r += s1();
  }
  NI int s2(void) { return (int)sizeof(T) * 1000 + K * 10; }
  return r + s2();
}

/* a nested function a replayed callee defines ends with the replay */
static inline int P(int a)
{
  int q(int y) { return y * 2; }
  return q(a);
}
NI int rp(void)
{
  int r = P(3);
  return r + q(1) * 10;
}

static inline int P2(int a)
{
  NI int q(int y) { return y * 2; }
  NI int p(int y) { return q(y) + 1; }
  int (*volatile fp)(int) = p;
  return fp(a);
}
NI int rp2(void)
{
  NI int q(int y) { return y + 1000; }
  int r = P2(3);
  NI int s(int y) { return q(y) + 7; }
  int (*volatile sp)(int) = s;
  return r + sp(1);
}

int main(void)
{
  printf("%d %d %d %d\n", j1(0), j2(0), st(0), en(0));
  printf("%d %d %d %d\n", lq(), ob(), twice_parent(1), after_parent(4));
  printf("%d %d %d %d\n", ea(2), ec(2), fw(0), ps(0));
  printf("%d %d %d %d\n", sl(), sib(), rp(), rp2());
  return 0;
}
