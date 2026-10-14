/*
 *  TCC Tests - __builtin_va_arg_pack() argument shapes and forwarding
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* Companion of bug_va_arg_pack_caller_scope_args: structs, bit-fields,
 * arrays, compound literals and functions in the pack, a pack used twice
 * (evaluated once), forwarded through a second wrapper, into printf/fprintf,
 * from a nested function, and in sizeof / dead code (no clone). */
#include <stdio.h>
#include <stdarg.h>
struct P { int a, b, c; };
struct B { unsigned x : 3, y : 5; };
int g = 5;
__attribute__((noinline)) int vs(const char *f, ...)
{
  va_list ap; int s = 0; va_start(ap, f);
  for (; *f; f++) {
    if (*f == 'i') s = s * 10 + va_arg(ap, int);
    else if (*f == 'p') { struct P p = va_arg(ap, struct P); s = s * 1000 + p.a * 100 + p.b * 10 + p.c; }
    else if (*f == 'a') { int *q = va_arg(ap, int *); s = s * 100 + q[0] * 10 + q[1]; }
    else if (*f == 'f') { int (*fp)(int) = va_arg(ap, int (*)(int)); s = s * 10 + fp(2); }
  }
  va_end(ap); return s;
}
static int sq(int v) { return v * v; }
static int sum(int n, ...) { va_list ap; va_start(ap, n); int s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
static inline __attribute__((always_inline)) int w(const char *f, ...) { return vs(f, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int w2(const char *f, ...) { return vs(f, __builtin_va_arg_pack()) + vs(f, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wo(const char *f, ...) { return w(f, __builtin_va_arg_pack()) + 1; }
static inline __attribute__((always_inline)) int wp(const char *f, ...) { return printf(f, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wfp(FILE *fp, const char *f, ...) { return fprintf(fp, f, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wn(int n, ...) { return sum(n, __builtin_va_arg_pack()); }
static inline __attribute__((always_inline)) int wonly(...) { return vs("ii", __builtin_va_arg_pack()); }
#define CALLW(...) w(__VA_ARGS__)
int evals;
__attribute__((noinline)) int nx(int v) { evals++; return v; }
int f4(int a) { int g = 100; __attribute__((noinline)) int nf(int y) { return wn(1, y) + g; } return nf(a); }
int main(void)
{
  struct P p = { 1, 2, 3 };
  struct B b = { 5, 17 };
  int arr[2] = { 4, 6 };
  int x = 9;
  printf("%d\n", w("pi", p, x));
  printf("%d\n", w("ia", b.x, arr));
  printf("%d\n", w("ai", (int[]){ 7, 8 }, b.y));
  printf("%d\n", w("fi", sq, sizeof(x)));
  printf("%d evals=%d\n", w2("ii", nx(1), nx(2)), evals);
  printf("%d\n", wo("iii", x, x + 1, x - 1));
  wp("%d %s %c\n", x, "str", 'q');
  wfp(stdout, "%d-%d\n", x, arr[1]);
  printf("%d\n", CALLW("ii", x, 3));
  printf("%d\n", wonly(x, 2));
  printf("%d %d\n", f4(1), (int)sizeof(w("i", x)));
  if (0) w("i", x);
  int z = 0 && w("i", x);
  printf("%d %d\n", z, ({ int q = 3; w("ii", q, q); }));
  return 0;
}
