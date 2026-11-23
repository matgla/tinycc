/*
 *  TCC Tests - computed goto: the CFG's edges into address-taken labels
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The CFG gave an IJUMP no successors and a label no block of its own, so
 * `s = 0` dominated the read of s after `top:` and ssa:load_cse forwarded the
 * 0 into a sanitized `s += 100` reached again through `goto *tab[0]`.  An
 * IJUMP's block now leads to every label's.  test_sanitize.py runs this with
 * -fsanitize=signed-integer-overflow; plain, it pins the unsanitized code. */
#include <stdio.h>

int arr[4];
static int st;

__attribute__((noinline)) int run(int n)
{
  static void *tab[] = {&&top, &&mid, &&done};
  int s = 0;
top:
  st++;
  if (st & 1)
    goto *tab[1];
  s += 100;
mid:
  for (int i = 0; i < 4; i++)
    arr[i] += st;
  if (st < n)
    goto *tab[0];
  goto *tab[2];
done:
  return s;
}

/* a value carried round a computed-goto loop in a register-sized local */
__attribute__((noinline)) int dispatch(const unsigned char *code)
{
  static void *ops[] = {&&op_add, &&op_dbl, &&op_end};
  int acc = 1;
  int pc = 0;
  goto *ops[code[pc]];
op_add:
  acc += 3;
  goto *ops[code[++pc]];
op_dbl:
  acc *= 2;
  goto *ops[code[++pc]];
op_end:
  return acc * 10 + pc;
}

static int sk;

__attribute__((noinline)) int skip(int n)
{
  static void *t[] = {&&a, &&b};
  int x = 7;
  int k = 0;
a:
  k++;
  if (k > n)
    return x;
  if (k & 1)
    goto *t[1];
  x -= 2;
b:
  x += k;
  sk += x;
  goto *t[0];
}

int main(void)
{
  int r = run(5);
  printf("run %d %d %d %d\n", r, st, arr[0], arr[3]);
  static const unsigned char prog[] = {0, 1, 0, 1, 1, 2};
  printf("dispatch %d\n", dispatch(prog));
  r = skip(5);
  printf("skip %d %d\n", r, sk);
  return 0;
}
