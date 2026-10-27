/*
 *  TCC Tests - __builtin_apply_args block must stay inside the frame
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* No IR operand names the 20-byte block __builtin_apply_args saves r0-r3
 * into, so at -O1+ the frame was sized past it and the block sat below SP:
 * any call (or an exception frame on the device) overwrote it before
 * __builtin_apply replayed it.  ra_reserved_frame_floor now keeps it. */
#include <stdio.h>

int G;

__attribute__((noinline)) void set_pv(int *p, int v) { *p = v; }

/* Fills its own frame, which with the bug overlaps the caller's block. */
__attribute__((noinline)) int scribble(int seed)
{
  volatile int junk[32];
  for (int i = 0; i < 32; i++)
    junk[i] = seed + i;
  return junk[seed & 31];
}

__attribute__((noinline)) int apply_after_call(int *p, int v)
{
  void *args = __builtin_apply_args();
  int before = *p;
  int s = scribble(0x5a5a);
  __builtin_apply((void (*)())set_pv, args, 16);
  return (before * 1000 + *p) + (s != 0x5a5a + 0x1a);
}

/* No call in between: the block must survive the plain path too. */
__attribute__((noinline)) int apply_direct(int *p, int v)
{
  void *args = __builtin_apply_args();
  int before = *p;
  __builtin_apply((void (*)())set_pv, args, 16);
  return before * 1000 + *p;
}

int main(void)
{
  G = 3;
  printf("after_call %d\n", apply_after_call(&G, 7));
  G = 4;
  printf("direct %d\n", apply_direct(&G, 9));
  printf("G %d\n", G);
  return 0;
}
