/*
 *  TCC IR - -fyasc-facts past 65536 calls in a function
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The IR keeps a call id in 16 bits, the facts were kept by the full id:
 * call 65536 (keep, whose result borrows x) read the facts of call 0
 * (fill, a no-escape view), so yf_promote kept x in a register and *q read
 * a stale slot.  Past 0xFFFF calls a function takes no facts. */
#pragma yasc

int printf(const char *fmt, ...);

static int nops;

__attribute__((noinline)) static void nop(void)
{
  nops++;
}

__attribute__((noinline)) static void fill(inout int *p)
{
  *p = 7;
}

__attribute__((noinline)) static int * borrows(p) keep(inout int *p)
{
  return p;
}

#define C4 nop(); nop(); nop(); nop();
#define C16 C4 C4 C4 C4
#define C256 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16 C16
#define C4096 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256 C256
#define C15(x) x x x x x x x x x x x x x x x

/* call 0 is fill, then 65535 calls of nop, then call 65536 is keep */
__attribute__((noinline)) static int f(void)
{
  int x = 1;
  fill(&x);
  C15(C4096) C15(C256) C15(C16) C4 C4 C4 nop(); nop(); nop();
  int * view q = keep(&x);
  x = 5;
  return *q * 100 + x;
}

int main(void)
{
  int r = f();
  printf("f %d nops %d\n", r, nops);
  return 0;
}
