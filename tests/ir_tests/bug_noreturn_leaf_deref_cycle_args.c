/*
 *  Regression test: a noreturn call from a leaf function whose arguments are
 *  loads through each other's registers (a deref cycle among r0-r3).
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Nothing is live across a noreturn call, so the pointer parameters stay in
 * r0-r3, the very registers the arguments go to.  die4(*q, *p, *t, *r) with
 * p..t in r0..r3 needs `ldr r2, [r3]` and `ldr r3, [r2]` both through the old
 * registers; the argument mover ran the first, then loaded through the value
 * it had just written (`ldr r2, [r3]; ldr r3, [r2]`).  The shapes below are a
 * two-register swap, the l3 shape, a three-cycle, a four-rotation, a 64-bit
 * argument by address, and an ordinary call with the pointers dead after it. */
#include <stdio.h>
#include <setjmp.h>

static jmp_buf jb;
static int got[4];

__attribute__((noreturn, noinline)) void die4(int a, int b, int c, int d)
{
  got[0] = a; got[1] = b; got[2] = c; got[3] = d;
  longjmp(jb, 1);
}

__attribute__((noreturn, noinline)) void diel(long long a, int b, int c)
{
  got[0] = (int)a; got[1] = (int)(a >> 32); got[2] = b; got[3] = c;
  longjmp(jb, 2);
}

__attribute__((noinline)) int sink4(int a, int b, int c, int d)
{
  got[0] = a; got[1] = b; got[2] = c; got[3] = d;
  return a - b + c - d;
}

__attribute__((noinline)) int swap2(int *p, int *q, int c, int d) { if (*p > c) die4(*q, *p, c, d); return *p + *q; }
__attribute__((noinline)) int cyc2(int *p, int *q, int *r, int *t) { if (*p > *q) die4(*q, *p, *t, *r); return *r - *t; }
__attribute__((noinline)) int cyc3(int *p, int *q, int *r, int d) { if (*p > d) die4(*q, *r, *p, d); return *p - d; }
__attribute__((noinline)) int rot4(int *p, int *q, int *r, int *t) { if (*p > *t) die4(*q, *r, *t, *p); return *q + *r; }
__attribute__((noinline)) int wide(int *p, long long *lp, int *r) { if (*p > *r) diel(*lp, *r, *p); return *p + *r; }
__attribute__((noinline)) int tail4(int *p, int *q, int *r, int *t) { return sink4(*q, *p, *t, *r); }

static int A[4] = {40, 30, 20, 10};
static long long L = 0x1122334455667788LL;

#define T(e) do { int v_, r_; got[0] = got[1] = got[2] = got[3] = 0; \
    if ((r_ = setjmp(jb)) == 0) { v_ = (e); printf("%-6s ret %d", #e, v_); } \
    else printf("%-6s jmp %d", #e, r_); \
    printf(" got %d %d %d %d\n", got[0], got[1], got[2], got[3]); } while (0)

int main(void)
{
  T(swap2(&A[0], &A[1], 5, 6));
  T(cyc2(&A[0], &A[1], &A[2], &A[3]));
  T(cyc3(&A[0], &A[1], &A[2], 7));
  T(rot4(&A[0], &A[1], &A[2], &A[3]));
  T(wide(&A[0], &L, &A[3]));
  T(tail4(&A[0], &A[1], &A[2], &A[3]));
  return 0;
}
