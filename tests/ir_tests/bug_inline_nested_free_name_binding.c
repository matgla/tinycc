/*
 * Regression test: below the first inline level the shadow check only looked
 * at enum constants, so a callee's free names -- typedefs, variables, nested
 * functions -- bound to locals of the enclosing inlined body, and an inlined
 * cleanup read a caller's struct tag.  Fix: a name the callee does not declare
 * itself must not resolve to a caller local (inline_body_free_names_unsafe).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

typedef int T;
static int g = 5;
enum { K = 7 };
static const int kc = 7;
int gx = 40;

static T scale(T x) { T s = x * 3; return s; }
static int outer_t(int v) { typedef char T; T c = 1; return scale(v) + c; }
static __attribute__((noinline)) int outer_t_ni(int v) { typedef char T; T c = 1; return scale(v) + c; }

static int get(int x) { return x + g; }
int mid(int x) { int g = 1000; return get(x) * 2 + g; }

static int addk(int x) { return x + K; }
int enum_outer(int x) { enum { K = 70 }; return addk(x) + K; }
int enum_outer_named(int x) { enum E2 { K = 700 }; return addk(x) + K; }

static int tsz(int n) { return n * (int)sizeof(T); }
static int outer_sz(void) { typedef char T[3]; T a = {1, 2, 3}; return tsz(5) + a[2]; }
static int addkc(int x) { return x + kc; }
static int outer_kc(void) { const int kc = 100; return addkc(1) + kc; }

static int pget(int x) { return x - g; }
static int outer_p(int g) { return pget(g) * 10 + g; }

static int leaf(int x) { T t = (T)x; return t + g; }
static int middle(int x) { typedef unsigned char T; int g = 3; T b = (T)x; return leaf(x) + b + g; }
static int top(int x) { return middle(x) + 1; }

static int rdx(int x) { extern int gx; return x + gx; }
static int outer_x(int x) { int gx = 4000; return rdx(x) + gx; }

static inline __attribute__((always_inline)) int ai_get(int x) { return x * g; }
static int outer_ai(int x) { int g = 11; return ai_get(x) + g; }

static int cleaned;
static void note(int *p) { cleaned += *p + g; }
static int outer_cl(int x) { int g = 600; { __attribute__((cleanup(note))) int c = x; } return g; }
struct S { int a, b; };
static void note_s(int *p) { cleaned += (int)sizeof(struct S) * *p; }
static __attribute__((noinline)) int tag_cl(int x)
{
  struct S { char c; } v = {1};
  { __attribute__((cleanup(note_s))) int c = x; }
  return v.c;
}

typedef long long FT;
static int ftsz(int n) { return n * (int)sizeof(FT); }
static int outer_ft(int n) { typedef int FT(void); return ftsz(n) + 1; }

static int qf(int y) { return y + 1000; }
static int calls_qf(int y) { return qf(y) + 1; }
static int outer_nf(int a) { int qf(int y) { return y * 2; } return calls_qf(a) + qf(a); }
static __attribute__((noinline)) int after_nf(int a) { return calls_qf(a); }

/* Scopes the callee's own declarations end: the later use is the global. */
int gi = 40;
static int sc1(int x) { { int gi = 1; x += gi; } return x + gi; }
static int sc2(int n) { int s = 0; for (int gi = 0; gi < n; gi++) s += gi; return s + gi; }
static int sc3(int n) { int s = 0; for (T gi = 0; gi < n; gi++) if (gi & 1) s += gi; else s -= gi; return s * 100 + gi; }
static int sc4(int n) { int s = 0; for (int gi = 0; gi < n; gi++) for (int j = 0; j < gi; j++) s += j; s += gi; return s; }
static int sc5(int x) { int y = gi; int gi = 3; return x + y + gi; }
static int sc6(int x) { int r = ({ int gi = 3; gi + x; }); return r + gi; }
static int sc7(int x) { if (x > 100) goto gi; return x + gi; gi: return 0; }
static int sc8(int x) { T y = x; return y + 1; }
static int scopes(int n)
{
  int gi = 1000, j = 5, T = 9;
  return sc1(n) + sc2(n) + sc3(n) + sc4(n) + sc5(n) + sc6(n) + sc7(n) + sc8(n) + gi + j + T;
}
/* ... and where they do not: a member, the callee's own loop, still inline. */
static int own(struct S *p) { int s = 0; for (int i = 0; i < p->a; i++) s += i * 2; return s + p->b; }
static int owner(int n) { int a = 7, b = 9, i = 100, s = 1; struct S v = {n, n}; return own(&v) + a + b + i + s; }

/* Depth 0: the caller's own locals shadow the callee's free names. */
int direct(int x)
{
  typedef short T;
  int g = 2000;
  enum { K = 9000 };
  T t = (T)x;
  return scale(70000) + get(x) + addk(x) + t + g + K;
}

/* The callee's own names may still match the outer body's: chains collapse. */
static int h3(int x) { int y = x + 1; return y * 2; }
static int h2(int x) { int y = h3(x) + 3; return y; }
static int h1(int x) { int y = h2(x) * 5; return y + x; }

int main(void)
{
  volatile int v = 300;
  volatile int one = 1;
  printf("typedef %d %d\n", outer_t(v), outer_t_ni(v));
  printf("variable %d\n", mid(one));
  printf("enum %d %d\n", enum_outer(one), enum_outer_named(one));
  printf("const %d %d\n", outer_sz(), outer_kc());
  printf("param %d\n", outer_p(one * 50));
  printf("depth2 %d\n", top(v));
  printf("extern %d\n", outer_x(one));
  printf("always_inline %d\n", outer_ai(one * 3));
  int cl = outer_cl(one * 2);
  printf("cleanup %d %d\n", cl, cleaned);
  cl = tag_cl(one * 10);
  printf("cleanup tag %d %d\n", cl, cleaned);
  printf("functions %d %d %d\n", outer_ft(one * 2), outer_nf(one * 3), after_nf(one * 4));
  printf("scopes %d\n", scopes(one * 4));
  printf("own %d\n", owner(one * 4));
  printf("direct %d\n", direct(one * 2));
  printf("chain %d\n", h1(v));
  return 0;
}
