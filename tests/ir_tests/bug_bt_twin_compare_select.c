/*
 *  TCC IR - a join that repeats the compare its incoming branches made
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* ra:branch_thread: a join whose CMP repeats the compare of the branch each
 * edge came through (operands unchanged) is decided on that edge; a SELECT
 * there becomes a move on each edge.  An inline CAS kept as a value is the
 * frontend's shape of it; test_bt_twin_compare_codegen.py checks the shapes.
 * Pinned here: both outcomes, an operand changed on one path, a weaker or
 * other-signedness condition, flags read again after the SELECT, three edges. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define ACQUIRE 2
#define SEQ_CST 5

volatile unsigned sink;

__attribute__((noinline)) int side(int v)
{
  sink += 100;
  return v + 1;
}

/* A CAS's result selected after the failure path joins. */
__attribute__((noinline)) int cs_take5(uint32_t *l, uint32_t token)
{
  uint32_t expected = 5;
  return __atomic_compare_exchange_n(l, &expected, token, false, SEQ_CST, RELAXED) ? 1 : 2;
}

/* Two registers selected: the desired value, or the old one written back. */
__attribute__((noinline)) uint32_t cs_sel_regs(uint32_t *l, uint32_t e, uint32_t d)
{
  return __atomic_compare_exchange_n(l, &e, d, false, ACQUIRE, ACQUIRE) ? d + 1000 : e;
}

/* The selected value stored, the expected one too big for CMP's immediate. */
__attribute__((noinline)) int cs_sel_store(uint32_t *l, uint32_t token, int *out)
{
  uint32_t expected = 0xabcdef01u;
  *out = __atomic_compare_exchange_n(l, &expected, token, false, ACQUIRE, RELAXED) ? 7 : 9;
  return (int)(expected & 0xff);
}

/* The failure path's write-back read by a branch, the result by a select. */
__attribute__((noinline)) int cs_branch_vreg(uint32_t *l, uint32_t e, uint32_t d)
{
  if (!__atomic_compare_exchange_n(l, &e, d, false, SEQ_CST, SEQ_CST))
  {
    sink += e;
    return -1;
  }
  return (int)e * 2;
}

/* No atomics: a call's result compared twice, a select at the join. */
__attribute__((noinline)) int tw_select(int x)
{
  int a = side(x);
  if (a < 7)
    sink += (unsigned)a;
  return a < 7 ? 10 : 20;
}

/* Two registers compared twice. */
__attribute__((noinline)) int tw_vreg(int x, int y)
{
  int a = side(x), b = side(y);
  if (a != b)
    sink += 1;
  return a == b ? 3 : 4;
}

/* The operand changes on one path: the join's compare reads the new value. */
__attribute__((noinline)) int tw_changed(int x)
{
  int a = side(x);
  if (a < 7)
    a = side(a + 10);
  return a < 7 ? 1 : 2;
}

/* A weaker condition (<= after <) and the other signedness: not decided. */
__attribute__((noinline)) int tw_weaker(int x, int y)
{
  int a = side(x), b = side(y);
  if (a < b)
    sink += 2;
  return a <= b ? 5 : 6;
}

__attribute__((noinline)) int tw_signedness(int x, int y)
{
  int a = side(x), b = side(y);
  if ((unsigned)a < (unsigned)b)
    sink += 3;
  return a < b ? 7 : 8;
}

/* Two selects on one compare after the join. */
__attribute__((noinline)) int tw_two_selects(int x, int *out)
{
  int a = side(x);
  if (a > 3)
    sink += 4;
  int c = a > 3;
  *out = c ? 11 : 12;
  return c ? 13 : 14;
}

/* Registers selected, the join entered three ways. */
__attribute__((noinline)) int tw_three(int x, int p, int q)
{
  int a = side(x);
  if (a == 9)
    sink += 5;
  else if (p > 2)
    sink += 6;
  return a == 9 ? p : q;
}

/* The selected value read by a compare right after. */
__attribute__((noinline)) int tw_select_then_test(int x)
{
  int a = side(x);
  if (a >= 4)
    sink += 7;
  int r = a >= 4 ? x : -x;
  if (r > 0)
    return r + 100;
  return r;
}

/* In a loop: the operand changes on the back edge into the join. */
__attribute__((noinline)) int tw_loop(int x)
{
  int a = side(x), acc = 0;
  for (int i = 0; i < 4; i++)
  {
    if (a < 110)
      sink += 1;
    acc += a < 110 ? 1 : 1000;
    a = side(a);
  }
  return acc;
}

/* The join is a loop header entered from a decided compare and a back edge. */
__attribute__((noinline)) int tw_header(int x)
{
  int a = side(x), n = 0;
  if (a > 5)
    sink += 1;
  do
  {
    n += a > 5 ? 2 : 3;
    if (n > 20)
      break;
    a = side(a - 2);
  } while (n < 50);
  return n;
}

int main(void)
{
  uint32_t w;
  int out, r, r2;

  w = 5;
  r = cs_take5(&w, 6);
  r2 = cs_take5(&w, 7);
  printf("take5: %d %d w=%u\n", r, r2, w);
  w = 40;
  printf("sel regs: %u", cs_sel_regs(&w, 40, 41));
  printf(" %u w=%u\n", cs_sel_regs(&w, 40, 42), w);
  w = 0xabcdef01u;
  r = cs_sel_store(&w, 3, &out);
  printf("sel store: %d %d w=%x", r, out, w);
  r = cs_sel_store(&w, 4, &out);
  printf(" %d %d w=%x\n", r, out, w);
  w = 12;
  sink = 0;
  r = cs_branch_vreg(&w, 12, 13);
  r2 = cs_branch_vreg(&w, 12, 14);
  printf("branch vreg: %d %d w=%u sink=%u\n", r, r2, w, sink);

  int a, b, c;
  sink = 0;
  a = tw_select(5), b = tw_select(6), c = tw_select(-9);
  printf("select: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_vreg(1, 1), b = tw_vreg(1, 2), c = tw_vreg(-3, -3);
  printf("vreg: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_changed(5), b = tw_changed(-20), c = tw_changed(6);
  printf("changed: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_weaker(1, 2), b = tw_weaker(2, 2), c = tw_weaker(3, 2);
  printf("weaker: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_signedness(-5, 2), b = tw_signedness(2, -5), c = tw_signedness(1, 2);
  printf("signedness: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_two_selects(3, &out);
  printf("two selects: %d %d", a, out);
  a = tw_two_selects(1, &out);
  printf(" %d %d sink=%u\n", a, out, sink);
  sink = 0;
  a = tw_three(8, 3, 4), b = tw_three(1, 3, 4), c = tw_three(1, 1, 4);
  printf("three: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_select_then_test(3), b = tw_select_then_test(2), c = tw_select_then_test(-9);
  printf("select then test: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_loop(105), b = tw_loop(300), c = tw_loop(-1);
  printf("loop: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = tw_header(4), b = tw_header(9), c = tw_header(5);
  printf("header: %d %d %d sink=%u\n", a, b, c, sink);
  return 0;
}
