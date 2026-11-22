/*
 *  TCC Tests - inline asm operands in registers the asm clobbers
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* An "m" operand's base -- a pointer the register allocator put in r5 -- was
 * used as `[r5]` by an asm that clobbers r5, and an "=r" output whose home is
 * a callee-saved register the asm clobbers was copied there inside the asm's
 * register save and lost to its restore.  A clobbered base is now copied to a
 * free register first; such an output is stored into the save slot. */
#include <stdio.h>

int store[8];
unsigned src[24], sink[8];

__attribute__((noinline)) int *getp(int i) { return &store[i]; }
__attribute__((noinline)) void use(unsigned *p) { p[0] = p[1] = 3; }

__attribute__((noinline)) int mem_base(int x)
{
  int *p = getp(4);
  asm volatile("mov r4, #1\n\tmov r5, #2\n\tmov r6, #3\n\tstr %1, %0" : "=m"(*p) : "r"(x) : "r4", "r5", "r6");
  return store[4];
}

/* the base in a caller-saved register the asm clobbers */
__attribute__((noinline)) int mem_base_low(int x)
{
  int *p = getp(2);
  asm volatile("mov r0, #0\n\tmov r1, #0\n\tmov r2, #0\n\tmov r3, #0\n\tstr %1, %0"
               : "=m"(*p) : "r"(x) : "r0", "r1", "r2", "r3");
  return store[2];
}

/* read through it too ("+m") */
__attribute__((noinline)) int mem_base_rw(int x)
{
  int *p = getp(6);
  *p = 40;
  asm volatile("mov r4, #0\n\tmov r5, #0\n\tmov r6, #0\n\tldr r7, %0\n\tadd r7, r7, %1\n\tstr r7, %0"
               : "+m"(*p) : "r"(x) : "r4", "r5", "r6", "r7");
  return store[6];
}

__attribute__((noinline)) unsigned clobbering(void)
{
  unsigned v0 = src[0], v1 = src[1], v2 = src[2], v3 = src[3], v4 = src[4], v5 = src[5];
  unsigned v6 = src[6], v7 = src[7], v8 = src[8], v9 = src[9], v10 = src[10], v11 = src[11];
  unsigned a = src[15] * 7, out;
  unsigned buf[4];
  use(buf);
  __asm volatile("mov r4, #1\n\tmov r5, #2\n\tadd %0, %1, r4\n\tadd %0, %0, r5"
                 : "=r"(out) : "r"(a) : "r4", "r5", "r6", "r7", "r8");
  use(buf);
  sink[5] = out;
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + a + out;
}

int main(void)
{
  for (int i = 0; i < 24; i++)
    src[i] = i + 1;
  printf("mem %d low %d rw %d\n", mem_base(99), mem_base_low(77), mem_base_rw(2));
  unsigned r = clobbering();
  printf("clob out=%u sum=%u\n", sink[5], r);
  return 0;
}
