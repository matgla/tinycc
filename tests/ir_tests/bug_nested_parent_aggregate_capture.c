/*
 *  TCC IR - a parent's captured aggregates are shared with its nested functions
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A nested function reads and writes its parent's struct and array locals
 * through the static chain, an access no escape analysis of the parent can
 * see.  Base f6d53136, -O1+: the parent's initialising stores were deleted
 * (dse) or its reads after a call forwarded from them (const_local_table,
 * entry_store, sccp), and the nested function read a member at an interior
 * offset from its own frame.  Fix: a frame shared with nested functions
 * keeps those passes out, and every interior offset of a capture names it. */
#include <stdio.h>
#include <string.h>
#define NI __attribute__((noinline))

struct S { int a, b; } gst = {7, 8};
int garr[3] = {9, 10, 11};
int fst(int a) { struct S gst = {70, 80}; NI int nf(int y) { return y + gst.a; } int (*volatile p)(int) = nf; return p(a); }
int farr(int a) { int garr[3] = {90, 100, 110}; NI int nf(int y) { return y + garr[2]; } int (*volatile p)(int) = nf; return p(a); }
struct S2 { int a, b; } g2 = {7, 8};
int arr2[3] = {9, 10, 11};
int fst2(int a) { struct S2 x = {70, 80}; NI int nf(int y) { return y + x.a + g2.b; } int (*volatile p)(int) = nf; return p(a); }
int farr2(int a) { int x[3] = {90, 100, 110}; NI int nf(int y) { return y + x[2] + arr2[1]; } int (*volatile p)(int) = nf; return p(a); }

NI int call(int (*f)(int), int x) { return f(x); }
struct P { int a, b; };
union U { int i; short s[2]; };
struct BF { unsigned x : 3, y : 5; };
/* direct calls */
NI int d1(int a)
{
  struct P s = {1, 2};
  NI int w(int y) { s.b = y; return s.a; }
  int r = w(a);
  return r * 100 + s.b;
}
NI int d2(int a)
{
  int arr[4] = {1, 2, 3, 4};
  NI void w(int y) { arr[y] += 10; }
  int s = 0;
  for (int i = 0; i < 4; i++) { w(i); s += arr[i]; }
  return s + a;
}
NI int d3(int a)
{
  struct P s = {a, a + 1};
  NI int w(int y) { return s.a * 10 + s.b + y; }
  s.a = 5;
  int r = w(0);
  s.b = 9;
  return r * 1000 + w(1);
}
NI int d4(int a)
{
  int arr[3] = {a, 0, 0};
  NI void w(void) { arr[2] = arr[0] + arr[1]; }
  arr[1] = 7;
  w();
  return arr[2];
}
NI int d5(int a)
{
  struct P s;
  NI void w(int y) { s.a = y; s.b = y * 2; }
  w(a);
  struct P c = s;
  return c.a + c.b;
}
/* trampolines */
NI int t2(int a)
{
  struct P s = {1, 2};
  int w(int y) { s.b = y; return s.a; }
  int r = call(w, a);
  return r * 100 + s.b;
}
NI int t3(int a)
{
  int arr[4] = {1, 2, 3, 4};
  int w(int y) { arr[y] += 10; return 0; }
  int (*volatile p)(int) = w;
  int s = 0;
  for (int i = 0; i < 4; i++) { p(i); s += arr[i]; }
  return s + a;
}
NI int t6(int a)
{
  struct P s;
  int w(int y) { s.a = y; s.b = y * 2; return 0; }
  call(w, a);
  struct P c = s;
  return c.a + c.b;
}
NI long long t7(int a)
{
  long long v = a;
  char buf[8] = "abc";
  int w(int y) { v += y; buf[1] = 'X'; return 0; }
  call(w, 5);
  return v * 1000 + buf[1];
}
NI int p2(int a)
{
  int arr[5] = {0};
  NI void put(int i) { arr[i] = i * i; }
  int total = 0;
  for (int i = 0; i < 5; i++) { put(i); total += arr[i]; }
  return total + a;
}
NI int p3(int a)
{
  union U u; u.i = 0;
  struct BF b = {1, 2};
  NI void w(void) { u.s[1] = 3; b.y = 17; }
  w();
  return u.i + b.y * 100000 + b.x;
}
NI int p4(int a)
{
  char buf[16];
  NI void w(void) { strcpy(buf, "hello"); }
  buf[0] = 0;
  w();
  return (int)strlen(buf) + a;
}
NI int p5(int a)
{
  int x = a;
  NI int sq(void) { return x * x; }
  int r = sq();
  x = 3;
  r += sq();
  x = 4;
  return r + sq();
}
NI int p6(int a)
{
  int v = 0;
  NI void set(int k) { v = k; }
  set(a);
  int r = v;
  set(a + 1);
  return r * 10 + v;
}
NI int p7(int a)
{
  struct P s = {0, 0};
  NI void w(void) { s.a++; s.b += 2; }
  for (int i = 0; i < a; i++) w();
  return s.a * 100 + s.b;
}
/* the nested function walks a captured array and struct by pointer, up to
 * one past the end */
NI int ptrs(int a)
{
  int arr[4] = {1, 2, 3, a};
  struct { char c; int v[3]; short t; } s = {'x', {10, 20, 30}, 7};
  NI int sum(void)
  {
    int t = 0;
    for (int *p = arr; p < arr + 4; p++) t += *p;
    for (int *p = &s.v[0]; p != &s.v[3]; p++) t += *p;
    return t + s.t + s.c;
  }
  NI void neg(void) { int *e = &arr[4]; while (e != arr) { --e; *e = -*e; } }
  int r = sum();
  neg();
  return r * 1000 + arr[0] + arr[3];
}

/* an inline-marked, self-recursive nested function reads captured members */
NI int selfrec(int k)
{
  struct P p = {k, 2 * k};
  int s(int n) { return n <= 0 ? p.b : s(n - 1) + p.a; }
  return s(6);
}

/* a capture takes no room in the nested function's frame: 200 levels of a
 * function capturing 16000 bytes stay far inside a 1 MiB stack */
NI int deep(int k)
{
  char big[16000];
  big[0] = (char)k;
  big[15999] = 2;
  NI int down(int n) { return n == 0 ? big[0] + big[15999] : down(n - 1) + 1; }
  return down(200);
}

int main(void)
{
  printf("%d %d %d %d\n", fst(1), farr(1), fst2(1), farr2(1));
  printf("%d %d %d %d %d\n", d1(3), d2(5), d3(1), d4(2), d5(4));
  printf("%d %d %d %lld\n", t2(5), t3(1), t6(3), t7(2));
  printf("%d %d %d %d %d %d\n", p2(1), p3(0), p4(2), p5(2), p6(4), p7(3));
  printf("%d %d %d\n", ptrs(9), selfrec(5), deep(3));
  return 0;
}
