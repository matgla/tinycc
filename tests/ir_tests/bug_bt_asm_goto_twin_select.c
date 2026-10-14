/*
 *  TCC IR - ra:branch_thread's twin compares and SELECT splits beside asm goto
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ra:branch_thread decides a join's repeated compare per edge and splits a
 * SELECT join into per-side move blocks put in front of the join.  An asm
 * goto's labels have no IR edge: a label on such a join, or on a block the
 * pass would sweep, would land on a side block or the next block.  The pass
 * skips functions with asm goto (func_has_asm_goto); these pin that for
 * every shape the twin-compare / SELECT threading handles: an inline CAS's
 * result selected after the join (-minline-atomics), a plain value compared
 * twice, a decided JUMPIF, three edges, the label on the join itself or on
 * a block only the asm goto enters.  Controls without asm goto still thread.
 * A join whose IR edges are all decided and that an asm goto on a path of its
 * own also enters is in bug_asm_goto_select_join.c. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#if defined(__arm__) || defined(__thumb__)
#define JEQ(x, k, L) __asm__ goto("cmp %0, #" #k "\n\tbeq %l[" #L "]" : : "r"(x) : "cc" : L)
#else
#define JEQ(x, k, L) __asm__ goto("cmpl $" #k ", %0\n\tje %l[" #L "]" : : "r"(x) : "cc" : L)
#endif

volatile unsigned sink;

__attribute__((noinline)) int side(int v)
{
  sink += 100;
  return v + 1;
}

/* The CAS's result selected after the failure path joins; the label is on
 * the join, so the asm goto is a hidden edge into the SELECT. */
__attribute__((noinline)) int ag_cas_label_on_join(uint32_t *l, uint32_t e, uint32_t d, int x)
{
  bool ok = __atomic_compare_exchange_n(l, &e, d, false, 5, 0);
  if (!ok)
  {
    sink += e;
    JEQ(x, 3, join);
    sink += 1000;
  }
join:
  return ok ? 10 + x : 20 + (int)e;
}

/* The CAS's result selected after the join; the label block is entered
 * only by the asm goto (failure path). */
__attribute__((noinline)) int ag_cas_label_only(uint32_t *l, uint32_t e, uint32_t d, int x)
{
  bool ok = __atomic_compare_exchange_n(l, &e, d, false, 2, 2);
  if (!ok)
  {
    sink += e;
    JEQ(x, 4, four);
  }
  return ok ? 1 : 2;
four:
  return 400 + (int)e;
}

/* The CAS's write-back tested by a branch, then the result selected: a
 * decided JUMPIF and a SELECT join, the asm goto between them. */
__attribute__((noinline)) int ag_cas_branch_select(uint32_t *l, uint32_t e, uint32_t d, int x)
{
  bool ok = __atomic_compare_exchange_n(l, &e, d, false, 5, 5);
  if (!ok)
    sink += 7;
  JEQ(x, 5, five);
  if (ok)
    sink += 11;
  return ok ? (int)d : (int)e;
five:
  return ok ? 500 : 600;
}

/* Plain twin compare and SELECT, the label on the join. */
__attribute__((noinline)) int ag_twin_select(int x, int y)
{
  int a = side(x);
  if (a < 7)
  {
    sink += (unsigned)a;
    JEQ(y, 2, j);
    sink += 3;
  }
j:
  return a < 7 ? 10 : 20;
}

/* Three edges into a SELECT on two registers, one of them the asm goto. */
__attribute__((noinline)) int ag_three(int x, int p, int q)
{
  int a = side(x);
  if (a == 9)
  {
    sink += 5;
    JEQ(p, 1, j);
  }
  else if (p > 2)
    sink += 6;
j:
  return a == 9 ? p : q;
}

/* A decided twin JUMPIF, its taken side a label only the asm goto also
 * enters. */
__attribute__((noinline)) int ag_twin_jumpif(int x, int y)
{
  int a = side(x);
  if (a > 4)
    sink += 1;
  JEQ(y, 7, seven);
  if (a > 4)
    goto big;
  return 1;
seven:
  return 70 + a;
big:
  return 2;
}

/* A CAS retry loop whose result is selected after an asm goto in the loop. */
__attribute__((noinline)) int ag_cas_loop(uint32_t *l, int x)
{
  uint32_t e = *l ^ (uint32_t)(x & 1);
  bool ok;
  int n = 0;
  do
  {
    ok = __atomic_compare_exchange_n(l, &e, e + 1, false, 5, 0);
    if (!ok)
    {
      n++;
      JEQ(x, 6, out);
    }
  } while (!ok && n < 3);
out:
  return ok ? n : -n - 1;
}

/* controls: the same CAS and twin shapes without asm goto */
__attribute__((noinline)) int ctl_cas(uint32_t *l, uint32_t e, uint32_t d, int x)
{
  bool ok = __atomic_compare_exchange_n(l, &e, d, false, 5, 0);
  if (!ok)
  {
    sink += e;
    if (x == 3)
      sink += 1000;
  }
  return ok ? 10 + x : 20 + (int)e;
}

__attribute__((noinline)) int ctl_twin(int x)
{
  int a = side(x);
  if (a < 7)
    sink += (unsigned)a;
  return a < 7 ? 10 : 20;
}

int main(void)
{
  for (int x = 1; x <= 7; x++)
    for (int s = 0; s < 2; s++)
    {
      uint32_t w = 40, w2, w3, w4, w5, w6;
      int r1, r2, r3, r4, r5, r6, r7, r8, r9;
      sink = 0;
      r1 = ag_cas_label_on_join(&w, s ? 41 : 40, 50, x);
      w2 = 40;
      r2 = ag_cas_label_only(&w2, s ? 39 : 40, 51, x);
      w3 = 40;
      r3 = ag_cas_branch_select(&w3, s ? 0 : 40, 52, x);
      r4 = ag_twin_select(x + 3 * s, x);
      r5 = ag_three(x + 4 * s, x, -x);
      r6 = ag_twin_jumpif(x + s, x);
      w4 = 7u * (unsigned)x;
      r7 = ag_cas_loop(&w4, x);
      w5 = 40;
      r8 = ctl_cas(&w5, s ? 41 : 40, 53, x);
      w6 = 0;
      r9 = ctl_twin(x + 3 * s);
      printf("x=%d s=%d: %d(%u) %d(%u) %d(%u) %d %d %d %d(%u) %d(%u) %d(%u) sink=%u\n", x, s, r1, w, r2, w2, r3,
             w3, r4, r5, r6, r7, w4, r8, w5, r9, w6, sink);
    }
  return 0;
}
