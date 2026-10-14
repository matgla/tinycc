/* ir/cfg.c gave a noreturn call (and TRAP) a fall-through edge, so a panic
 * guard's block joined the code after it and forced a reload; the call is now
 * a block terminator.  Also pins the SUB/CMP flag fusion across a store.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <setjmp.h>
#include <stdio.h>

struct S
{
  unsigned a, depth, flags;
};
struct S s_min;
static jmp_buf env;
static int panics;
static unsigned sunk;

__attribute__((noinline, noreturn)) void die(int code)
{
  panics += code;
  longjmp(env, code);
}

__attribute__((noinline)) void sink(unsigned v) { sunk += v + 1; }

/* The Zig panic-guard shape: the code after the guard must not reload depth. */
__attribute__((noinline)) unsigned guard_noreturn(void)
{
  struct S *p = &s_min;
  if (p->depth == 0) {
    die(42);
    __builtin_unreachable();
  }
  p->depth = p->depth - 1;
  return p->depth;
}

/* subs; str; cmp #0: the store between the SUB and the CMP is flag-neutral. */
__attribute__((noinline)) void dec_store_test(void)
{
  struct S *p = &s_min;
  unsigned v = p->depth - 1;
  p->depth = v;
  if (v != 0)
    return;
  unsigned f = p->flags;
  p->flags = 0;
  sink(f);
}

/* Values live across two guards and a loop guard. */
__attribute__((noinline)) int sum_checked(const int *a, int n, int lim)
{
  int base = a[0] * 3;
  if (n <= 0)
    die(1);
  int sum = 0;
  for (int i = 0; i < n; i++) {
    if (a[i] > lim)
      die(2);
    sum += a[i];
  }
  if (sum == 0)
    __builtin_trap();
  return sum + base;
}

/* The code after the noreturn call is also a goto target. */
__attribute__((noinline)) int after_label(int x)
{
  if (x < 0)
    goto L;
  if (x == 0)
    die(3);
  x += 10;
L:
  return x * 2;
}

/* noreturn in a switch default, with a value merged after the switch. */
__attribute__((noinline)) int pick(int k, int v)
{
  int r;
  switch (k) {
  case 0: r = v + 1; break;
  case 1: r = v * 5; break;
  case 2: r = v - 7; break;
  default: die(4);
  }
  return r ^ k;
}

/* A noreturn call returning a value: the result is never used on a live path. */
__attribute__((noinline, noreturn)) int die_val(int code)
{
  die(code);
}

__attribute__((noinline)) int val_guard(int x)
{
  int y = x * 7;
  if (x > 100)
    y = die_val(5);
  return y + x;
}

static int run(int which)
{
  volatile int code = setjmp(env);
  if (code)
    return -code;
  int arr[4] = {1, 2, 3, 4};
  switch (which) {
  case 0: return (int)guard_noreturn();
  case 1: dec_store_test(); return (int)s_min.depth;
  case 2: return sum_checked(arr, 4, 10);
  case 3: return sum_checked(arr, 4, 3);
  case 4: return sum_checked(arr, 0, 10);
  case 5: return after_label(-4);
  case 6: return after_label(0);
  case 7: return after_label(5);
  case 8: return pick(0, 10) + pick(1, 10) + pick(2, 10);
  case 9: return pick(9, 10);
  case 10: return val_guard(3);
  case 11: return val_guard(300);
  }
  return 0;
}

int main(void)
{
  int r;
  s_min.depth = 2;
  for (int i = 1; i <= 3; i++) {
    r = run(0);
    printf("g%d %d depth %u\n", i, r, s_min.depth);
  }
  s_min.depth = 2;
  s_min.flags = 9;
  for (int i = 1; i <= 2; i++) {
    r = run(1);
    printf("d%d %d flags %u sunk %u\n", i, r, s_min.flags, sunk);
  }
  for (int i = 2; i < 12; i++)
    printf("r%d %d\n", i, run(i));
  printf("panics %d\n", panics);
  return 0;
}
