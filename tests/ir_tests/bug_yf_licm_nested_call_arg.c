/*
 *  TCC IR - -fyasc-facts LICM and arguments placed before a nested call
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* `reset(b, k(i))` emits `PARAM b (reset#0); CALL k; PARAM T (reset#1);
 * CALL reset`.  licm.c fph_yf_call_writes looked for reset's arguments only
 * back to the previous call, missed b, and hoisted `b->data` (b a restrict
 *parameter) out of the loop although reset replaces it.  The
 * arguments are now found by call id wherever they are. */
#pragma yasc

int printf(const char *fmt, ...);

struct B
{
  int * view data;
  int n;
};

static int pool[4] = {10, 20, 30, 40};
static int calls;

__attribute__((noinline)) static int k(int i)
{
  calls++;
  return (i + 1) & 3;
}

__attribute__((noinline)) static void reset(inout struct B *b, int v)
{
  b->data = &pool[v];
  b->n++;
}

__attribute__((noinline)) static int peek(const struct B *b, int v)
{
  return b->n + v;
}

/* the argument before the nested call may be written */
__attribute__((noinline)) static int sum(inout struct B * restrict b, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    s += b->data[0];
    reset(b, k(i));
  }
  return s;
}

/* a read-only parameter with a nested call: b->data still hoists, same result */
__attribute__((noinline)) static int sum_ro(struct B * restrict b, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += b->data[0] + peek(b, k(i));
  return s;
}

int main(void)
{
  struct B b = {&pool[0], 0};
  int s = sum(&b, 3);
  printf("sum %d n %d\n", s, b.n);
  struct B c = {&pool[2], 5};
  printf("sum_ro %d\n", sum_ro(&c, 3));
  printf("calls %d\n", calls);
  return 0;
}
