/*
 *  Regression test: an indirect call whose target is also a register argument
 *  branches through that argument register.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `v(n, v)`: the allocator kept such a target out of r0-r3 (a callee-saved
 * push/pop per call) and the call lowering moved it to a holding register.
 * After the argument moves the slot register holds the target, so the call
 * now branches through it.  The dispatch and perm functions cover the allocator
 * hint landing (identity slot) and missing (target read by the move
 * scheduler before its register is overwritten).  test_codegen_asm.py checks
 * the code shape, this program the argument values. */
#include <stdio.h>

typedef int (*f1_t)(void *);
typedef int (*f2_t)(void *, void *);
typedef int (*f3_t)(void *, void *, void *);
typedef int (*f4_t)(void *, void *, void *, void *);
struct n1 { f1_t v; };
struct n2 { f2_t v; };
struct n3 { f3_t v; };
struct n4 { f4_t v; };

static int calls;
static char X, Y, Z;
static void *self_seen[4];

/* Each callee records its arguments; main compares them with the call. */
__attribute__((noinline)) int c1(void *a)
{
  calls++;
  self_seen[0] = a;
  return 1;
}

__attribute__((noinline)) int c2(void *a, void *b)
{
  calls++;
  self_seen[0] = a;
  self_seen[1] = b;
  return 2;
}

__attribute__((noinline)) int c3(void *a, void *b, void *c)
{
  calls++;
  self_seen[0] = a;
  self_seen[1] = b;
  self_seen[2] = c;
  return 3;
}

__attribute__((noinline)) int c4(void *a, void *b, void *c, void *d)
{
  calls++;
  self_seen[0] = a;
  self_seen[1] = b;
  self_seen[2] = c;
  self_seen[3] = d;
  return 4;
}

/* target in its own slot */
__attribute__((noinline)) int dispatch_slot1(struct n2 *n) { f2_t v = n->v; return v(n, (void *)v); }
__attribute__((noinline)) int dispatch_slot0(struct n1 *n) { f1_t v = n->v; return v((void *)v); }
__attribute__((noinline)) int dispatch_three(struct n4 *n, void *x) { f4_t v = n->v; return v((void *)v, (void *)v, x, (void *)v); }
__attribute__((noinline)) int dispatch_nontail(struct n2 *n) { f2_t v = n->v; return v(n, (void *)v) + 10; }

/* argument permutations: the target's register also feeds another slot */
__attribute__((noinline)) int perm1(struct n2 *a, void *b) { f2_t v = a->v; return v(b, (void *)v); }
__attribute__((noinline)) int perm2(void *b, struct n2 *a) { f2_t v = a->v; return v((void *)v, b); }
__attribute__((noinline)) int perm3(void *x, struct n3 *a, void *y) { f3_t v = a->v; return v(y, x, (void *)v); }
__attribute__((noinline)) int perm4(void *x, void *y, struct n4 *a, void *z) { f4_t v = a->v; return v(z, y, x, (void *)v); }
__attribute__((noinline)) int perm5(struct n4 *a, void *x, void *y, void *z) { f4_t v = a->v; return v(z, (void *)v, x, y); }
__attribute__((noinline)) int perm6(struct n2 *a, void *b) { f2_t v = a->v; return v((void *)v, a); }
__attribute__((noinline)) int perm7(void *x, void *y, struct n4 *a, void *z) { f4_t v = a->v; return v((void *)v, z, y, x); }
__attribute__((noinline)) int perm8(struct n4 *a, void *x, void *y, void *z) { f4_t v = a->v; return v(y, z, (void *)v, x); }

static int check(const char *name, int got, int want, void *e0, void *e1, void *e2, void *e3)
{
  void *e[4] = {e0, e1, e2, e3};
  int ok = got == want;
  for (int i = 0; i < want; i++)
    ok &= self_seen[i] == e[i];
  printf("%s=%s\n", name, ok ? "ok" : "BAD");
  for (int i = 0; i < 4; i++)
    self_seen[i] = 0;
  return ok;
}

int main(void)
{
  struct n1 s1 = {c1};
  struct n2 s2 = {c2};
  struct n3 s3 = {c3};
  struct n4 s4 = {c4};
  void *v1 = (void *)c1, *v2 = (void *)c2, *v3 = (void *)c3, *v4 = (void *)c4;
  int ok = 1;

  ok &= check("slot1", dispatch_slot1(&s2), 2, &s2, v2, 0, 0);
  ok &= check("slot0", dispatch_slot0(&s1), 1, v1, 0, 0, 0);
  ok &= check("three", dispatch_three(&s4, &X), 4, v4, v4, &X, v4);
  ok &= check("nontail", dispatch_nontail(&s2) - 10, 2, &s2, v2, 0, 0);
  ok &= check("perm1", perm1(&s2, &X), 2, &X, v2, 0, 0);
  ok &= check("perm2", perm2(&X, &s2), 2, v2, &X, 0, 0);
  ok &= check("perm3", perm3(&X, &s3, &Y), 3, &Y, &X, v3, 0);
  ok &= check("perm4", perm4(&X, &Y, &s4, &Z), 4, &Z, &Y, &X, v4);
  ok &= check("perm5", perm5(&s4, &X, &Y, &Z), 4, &Z, v4, &X, &Y);
  ok &= check("perm6", perm6(&s2, &X), 2, v2, &s2, 0, 0);
  ok &= check("perm7", perm7(&X, &Y, &s4, &Z), 4, v4, &Z, &Y, &X);
  ok &= check("perm8", perm8(&s4, &X, &Y, &Z), 4, &Y, &Z, v4, &X);
  printf("calls=%d\n", calls);
  return ok ? 0 : 1;
}
