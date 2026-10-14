/*
 *  TCC IR - a compare's flag tested again where the compare's branch joins
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `ok = a == b; if (a != b) {...}; if (ok)`: on each edge into the second test
 * the first branch already decided ok (opt/ra/branch_thread.c, the SETIF of a
 * repeated compare), so the edges go straight to the successor ok picks.  An
 * inline compare-exchange is the frontend's shape of this (inline_atomic_rmw);
 * test_cas_flag_join_codegen.py checks the shapes.  Pinned here: both
 * outcomes, a compare whose operand changes between the two compares, a flag
 * of a different condition, and a block entered by the branch only. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define RELAXED 0
#define ACQUIRE 2
#define SEQ_CST 5

volatile unsigned sink;
volatile uint32_t mmio;

/* The CAS's flag read again after the failure path, and stored. */
__attribute__((noinline)) int cas_flag_ne(uint32_t *l, uint32_t e, uint32_t token, int *out)
{
  bool ok = __atomic_compare_exchange_n(l, &e, token, false, SEQ_CST, SEQ_CST);
  if (!ok)
    sink += 3;
  *out = ok;
  return (int)e;
}

/* Both arms after the join; the failure path is entered by a branch. */
__attribute__((noinline)) int cas_flag_keep(uint32_t *l, uint32_t token, int *out)
{
  uint32_t e = 0;
  bool ok = __atomic_compare_exchange_n(l, &e, token, false, ACQUIRE, RELAXED);
  if (ok)
    sink += 1;
  else
    sink += 2;
  *out = ok;
  return (int)e;
}

/* Zig's cmpxchgWeak spin lock: the flag goes through a struct, WFE while
 * taken, in a function that touches volatile memory. */
struct opt_u32
{
  uint32_t payload;
  bool is_null;
};

__attribute__((noinline)) int cas_flag_spin(uint32_t *l, uint32_t token)
{
  int waits = 0;
  for (;;)
  {
    struct opt_u32 t12, t8;
    t12.payload = 0;
    t12.is_null = __atomic_compare_exchange_n(l, &t12.payload, token, true, ACQUIRE, RELAXED);
    t8 = t12;
    if (t8.is_null)
      break;
    sink = mmio;
    if (++waits == 3)
      __atomic_store_n(l, 0, SEQ_CST);
  }
  return waits;
}

__attribute__((noinline)) int plain_side(int);

/* Plain C: the same compare twice, its flag tested after the join. */
__attribute__((noinline)) int plain_twin(int x, int y)
{
  int ok = x < y;
  if (x >= y)
    sink += (unsigned)x;
  if (ok)
    return 10 + plain_side(x);
  return 20;
}

/* The same, on a call's result (a TEMP) against a constant. */
__attribute__((noinline)) int plain_twin_tmp(int x)
{
  int a = plain_side(x);
  int ok = a < 7;
  if (a >= 7)
    sink += (unsigned)a;
  if (ok)
  {
    sink += 1000;
    return 10;
  }
  return 20;
}

/* x changes between the compares: the second does not decide ok. */
__attribute__((noinline)) int plain_changed(int x, int y)
{
  int ok = x == y;
  x = plain_side(x);
  if (x != y)
    sink += 1;
  if (ok)
    return 30;
  return 40;
}

/* A flag of another condition: x <= y is not decided by x < y. */
__attribute__((noinline)) int plain_other_cond(int x, int y)
{
  int ok = x <= y;
  if (x < y)
    sink += 2;
  if (ok)
    return 50;
  return 60;
}

/* Unsigned and inverted: ok = x >= y, the branch tests x < y. */
__attribute__((noinline)) int plain_unsigned(unsigned x, unsigned y)
{
  int ok = x >= y;
  if (x < y)
    sink += 4;
  if (ok)
    return 70;
  return 80;
}

__attribute__((noinline)) int plain_side(int v)
{
  sink += 100;
  return v + 1;
}

int main(void)
{
  uint32_t w;
  int out, r;

  w = 7;
  r = cas_flag_ne(&w, 7, 8, &out);
  printf("ne hit: r=%d out=%d w=%u sink=%u\n", r, out, w, sink);
  r = cas_flag_ne(&w, 7, 9, &out);
  printf("ne miss: r=%d out=%d w=%u sink=%u\n", r, out, w, sink);

  w = 0;
  r = cas_flag_keep(&w, 5, &out);
  printf("keep free: r=%d out=%d w=%u sink=%u\n", r, out, w, sink);
  r = cas_flag_keep(&w, 6, &out);
  printf("keep busy: r=%d out=%d w=%u sink=%u\n", r, out, w, sink);

  w = 0;
  r = cas_flag_spin(&w, 11);
  printf("spin free: waits=%d w=%u\n", r, w);
  w = 1;
  r = cas_flag_spin(&w, 12);
  printf("spin taken: waits=%d w=%u\n", r, w);

  int a, b, c;
  sink = 0;
  a = plain_twin(1, 2), b = plain_twin(2, 2), c = plain_twin(3, 2);
  printf("twin: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = plain_twin_tmp(5), b = plain_twin_tmp(6), c = plain_twin_tmp(-9);
  printf("twin tmp: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = plain_changed(4, 4), b = plain_changed(3, 4), c = plain_changed(9, 4);
  printf("changed: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = plain_other_cond(1, 2), b = plain_other_cond(2, 2), c = plain_other_cond(3, 2);
  printf("other: %d %d %d sink=%u\n", a, b, c, sink);
  sink = 0;
  a = plain_unsigned(1u, 2u), b = plain_unsigned(2u, 2u), c = plain_unsigned(0xffffffffu, 2u);
  printf("unsigned: %d %d %d sink=%u\n", a, b, c, sink);
  return 0;
}
