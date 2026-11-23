/*
 *  TCC Tests - LICM hoisting a global's load past __builtin_apply
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* licm's memory-clobber test let IR_HZ_NONLOCAL ops through, so a loop whose
 * only write was `__builtin_apply` (a call of anything) counted as writing
 * no memory and ra:licm hoisted `int g_ = G;` out of it; the global-load
 * hoist (`s += G`) did not count it as a write either.  A non-local op now
 * clobbers memory in both. */
#include <stdio.h>

int G, H;
int arr[4];

__attribute__((noinline)) void set_pv(int *p, int v) { *p = v; }

__attribute__((noinline)) int loop_local(int *p, int v)
{
  void *args__ = __builtin_apply_args();
  int s = 0;
  for (int i = 0; i < 3; i++)
  {
    int g_ = G;
    int t_ = s * 10;
    s = t_ + g_;
    __builtin_apply((void (*)())set_pv, args__, 16);
  }
  return s;
}

/* the operand form (passed before) */
__attribute__((noinline)) int loop_operand(int *p, int v)
{
  void *args__ = __builtin_apply_args();
  int s = 0;
  for (int i = 0; i < 3; i++)
  {
    s = s * 10 + G;
    __builtin_apply((void (*)())set_pv, args__, 16);
  }
  return s;
}

/* the global-load hoist: a read consumed by an ALU op */
__attribute__((noinline)) int loop_sum(int *p, int v)
{
  void *args__ = __builtin_apply_args();
  int s = 0;
  for (int i = 0; i < v; i++)
  {
    s += G;
    __builtin_apply((void (*)())set_pv, args__, 16);
  }
  return s;
}

__attribute__((noinline)) int loop_while(int *p, int v)
{
  void *args__ = __builtin_apply_args();
  int s = 0;
  int i = 0;
  while (i < v)
  {
    s = s * 10 + G;
    i++;
    __builtin_apply((void (*)())set_pv, args__, 16);
  }
  return s;
}

/* an array element through a pointer */
__attribute__((noinline)) int loop_ptr(int *p, int v)
{
  void *args__ = __builtin_apply_args();
  int s = 0;
  for (int i = 0; i < 3; i++)
  {
    int x = *p;
    s = s * 10 + x;
    __builtin_apply((void (*)())set_pv, args__, 16);
  }
  return s;
}

/* a loop that writes nothing still hoists (must stay right) */
__attribute__((noinline)) int loop_plain(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    int h = H;
    s = s * 10 + h;
  }
  return s;
}

int main(void)
{
  G = 7;
  printf("local %d\n", loop_local(&G, 4));
  G = 7;
  printf("operand %d\n", loop_operand(&G, 4));
  G = 7;
  printf("sum %d\n", loop_sum(&G, 3));
  G = 7;
  printf("while %d\n", loop_while(&G, 3));
  arr[2] = 5;
  printf("ptr %d\n", loop_ptr(&arr[2], 1));
  H = 3;
  printf("plain %d\n", loop_plain(3));
  return 0;
}
