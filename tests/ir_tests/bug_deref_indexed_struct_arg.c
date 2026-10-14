/*
 *  TCC IR - A struct by-value argument read from a scaled-index array element
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* deref_indexed_fusion, licm_field_ptr and licm_global_load turned a struct
 * by-value argument `PARAM addr***DEREF***` into a one-word load, so the call
 * copied the struct from that word used as an address.  A STRUCT operand is an
 * address, not a loadable value: all three now skip it.  Scalar derefs must
 * still fold and hoist. */
#include <stdio.h>

struct S2 { short a; };
struct S4 { int a; };
struct S8 { const int *p; int n; };
struct H4 { unsigned short lo, hi; };

__attribute__((noinline)) int f2(struct S2 s) { return s.a; }
__attribute__((noinline)) int f4(struct S4 s) { return s.a; }
__attribute__((noinline)) int f8(struct S8 s) { return s.p[s.n - 1] * 10 + s.n; }
__attribute__((noinline)) int fh(struct H4 h) { return h.lo * 1000 + h.hi; }
__attribute__((noinline)) int two(struct S4 x, struct S4 y) { return x.a * 100 + y.a; }
__attribute__((noinline)) int late(int a, int b, int c, int d, struct S8 s, struct S2 t)
{
  return a + b + c + d + s.p[0] + s.n * 10 + t.a * 1000;
}
__attribute__((noinline)) int mid(int a, struct S4 s, int b) { return a * 100 + s.a * 10 + b; }

static const int k[] = {1, 2, 3};
struct S2 a2[3] = {{-5}, {6}, {7}};
struct S4 a4[3] = {{7}, {8}, {9}};
struct S8 a8[3] = {{k, 1}, {k, 2}, {k, 3}};
struct H4 ah[3] = {{1, 2}, {3, 4}, {65535, 6}};
int ints[4] = {3, 1, 4, 1};
unsigned short us[4] = {65535, 2, 40000, 7};
volatile int vzero;

__attribute__((noinline)) int sum_local(int n)
{
  struct S4 loc[4];
  struct S8 loc8[4];
  for (int i = 0; i < 4; i++) {
    loc[i].a = i * 3 + vzero;
    loc8[i].p = k;
    loc8[i].n = 1 + (i % 3);
  }
  int s = 0;
  for (int i = 0; i < n; i++)
    s += f4(loc[i]) + f8(loc8[i]);
  return s;
}

__attribute__((noinline)) int sum_table(int i)
{
  /* const_local_table promotes this to a .rodata table. */
  struct S4 t[4] = {{11}, {22}, {33}, {44}};
  struct S2 u[4] = {{1}, {-2}, {3}, {-4}};
  return f4(t[i]) + f2(u[i]);
}

__attribute__((noinline)) int pairs(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
      s += two(a4[i], a4[j]);
  return s;
}

__attribute__((noinline)) int scalar_reads(int n)
{
  /* Scalar element derefs used as operands: the fold must still apply. */
  int s = 0;
  for (int i = 0; i < n; i++) {
    if (ints[i] != i)
      s += ints[i] * 7;
    s += us[i] + 1;
  }
  return s;
}

struct O { int pad; struct S8 in; struct S4 q; };
struct O go = {1, {k, 3}, {4}};
struct S8 g8 = {k, 2};
struct S4 g4 = {5};
int gi = 6;
__attribute__((noinline)) int fi(int v) { return v; }

/* Loop-invariant struct arguments (the LICM hoists). */
__attribute__((noinline)) int glob_loop(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += f4(g4) * i;
  for (int i = 0; i < n; i++)
    s += f8(g8) * i;
  for (int i = 0; i < n; i++)
    s += fi(gi) * i;
  return s;
}
__attribute__((noinline)) int ptr_loop(const struct O *o, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += f8(o->in) + f4(o->q) * i;
  return s;
}
__attribute__((noinline)) int inv_idx(int j, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += two(a4[j], a4[i]) + f4(a4[j]) * i;
  return s;
}
__attribute__((noinline)) int arr_ptr(const struct S4 *p, const struct S8 *q, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += f4(p[i]) + f4(p[1]) * i + f8(q[2]) + f8(*q);
  return s;
}

/* Other shapes: reread, store then pass, unrolled constant indices, local
 * array, pointer base, 12-byte element, mixed args, byte index. */
struct S12 { int a, b, c; };
__attribute__((noinline)) int f12(struct S12 s) { return s.a + s.b * 10 + s.c * 100; }
__attribute__((noinline)) int mix(struct S4 x, int a, struct S4 y) { return x.a * 100 + a * 10 + y.a; }
struct S4 b4[4] = {{7}, {8}, {9}, {10}};
struct S2 b2[4] = {{-1}, {2}, {-3}, {4}};
struct S12 a12[3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
__attribute__((noinline)) int cse(int i) { int x = b4[i].a; return f4(b4[i]) + x + f4(b4[i]); }
__attribute__((noinline)) int fwd(int i, int v) { b4[i].a = v; return f4(b4[i]); }
__attribute__((noinline)) int unr(void) { int s = 0; for (int i = 0; i < 4; i++) s += f4(b4[i]) * (i + 1); return s; }
__attribute__((noinline)) int loc(int i)
{
  struct S4 l[4];
  for (int j = 0; j < 4; j++)
    l[j].a = j * j;
  return f4(l[i]) + f4(l[2]);
}
__attribute__((noinline)) int ptr(struct S4 *p, int i) { return f4(p[i]) + f4(p[i + 1]) + f4(*(p + 2)); }
__attribute__((noinline)) int p12(int i) { return f12(a12[i]); }
__attribute__((noinline)) int mixc(int i, int j) { return mix(b4[i], i + j, b4[j]); }
__attribute__((noinline)) int byte_idx(const unsigned char *idx, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += f8(a8[idx[i]]);
  return s;
}
__attribute__((noinline)) int sh2(struct S2 *p, int i) { return f2(p[i]) + f2(p[i ^ 1]); }

int main(void)
{
  int s2 = 0, s4 = 0, s8 = 0, sh = 0, sl = 0, sm = 0;
  for (int i = 0; i < 3; i++)
    s2 += f2(a2[i]);
  printf("s2 %d\n", s2);
  for (int i = 0; i < 3; i++)
    s4 += f4(a4[i]);
  printf("s4 %d\n", s4);
  for (int i = 0; i < 3; i++)
    s8 += f8(a8[i]);
  printf("s8 %d\n", s8);
  for (int i = 0; i < 3; i++)
    sh += fh(ah[i]);
  printf("sh %d\n", sh);
  for (int i = 0; i < 3; i++)
    sl += late(i, 1, 2, 3, a8[i], a2[i]);
  printf("late %d\n", sl);
  for (int i = 0; i < 3; i++)
    sm += mid(i, a4[i], i + 1);
  printf("mid %d\n", sm);
  printf("local %d\n", sum_local(4));
  for (int i = 0; i < 4; i++)
    printf("table %d %d\n", i, sum_table(i));
  printf("pairs %d\n", pairs(3));
  for (int i = 0; i < 3; i++) {
    struct S8 c = a8[i];
    printf("copy %d %d\n", c.p[0], c.n);
  }
  printf("scalar %d\n", scalar_reads(4));
  printf("glob %d\n", glob_loop(3));
  printf("ptr %d\n", ptr_loop(&go, 3));
  printf("inv %d\n", inv_idx(2, 3));
  printf("arrp %d\n", arr_ptr(a4, a8, 3));
  static const unsigned char idx[] = {2, 0, 1};
  printf("cse %d\n", cse(2));
  printf("unr %d\n", unr());
  printf("fwd %d\n", fwd(1, 42));
  printf("loc %d\n", loc(3));
  printf("ptr %d\n", ptr(b4, 1));
  printf("p12 %d\n", p12(2));
  printf("mixc %d\n", mixc(0, 3));
  printf("bidx %d\n", byte_idx(idx, 3));
  printf("sh2 %d\n", sh2(b2, 2));
  return 0;
}
