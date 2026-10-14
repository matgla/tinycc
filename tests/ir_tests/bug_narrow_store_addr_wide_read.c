/*
 *  TCC IR - A narrow store through a local's address is not its whole value
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* value_tracking and ssa:sccp took a STORE through `&V` as V's new value at
 * any width, so `*(char *)&x = 2` made the int x read back as 2.  A store
 * through &V now defines V only when it covers the read exactly; a narrower
 * read gets the stored bytes extended by its own signedness. */
#include <stdio.h>

typedef unsigned long long u64;

__attribute__((noinline)) int st8(int k) { int x = k; *(char *)&x = 2; return x; }
__attribute__((noinline)) u64 st8_64(int k) { u64 x = k; *(unsigned char *)&x = 9; return x; }
__attribute__((noinline)) u64 st32_64(int k) { u64 x = k; *(unsigned *)&x = 5; return x; }
__attribute__((noinline)) int st16(int k) { int x = k; *(short *)&x = 0x1234; return x; }
__attribute__((noinline)) int sh8(int k) { short x = k; *(char *)&x = 0x55; return x; }
__attribute__((noinline)) int sc_sign(int k) { signed char c = k; *(unsigned char *)&c = 0xff; return c; }
__attribute__((noinline)) int uc_sign(int k) { unsigned char c = k; *(signed char *)&c = -2; return c; }
__attribute__((noinline)) int hi8(int k) { int x = k; *((char *)&x + 1) = 0x77; return x; }
__attribute__((noinline)) int then_full(int k) { int x = k; *(char *)&x = 2; *(int *)&x = 11; return x; }
__attribute__((noinline)) int full(int k) { int x = k; *(int *)&x = 7; return x + 1; }
__attribute__((noinline)) int full_ptr(int k) { int x = k; int *p = &x; *p = 13; return x; }
__attribute__((noinline)) u64 full64(int k) { u64 x = k; *(u64 *)&x = 0x123456789ull; return x; }
__attribute__((noinline)) int narrow_br(int k) {
  int x = k;
  if (k > 0)
    *(char *)&x = 3;
  else
    *(char *)&x = 4;
  return x;
}

int main(void)
{
  printf("%d %llx %llx\n", st8(-3071), st8_64(-3), st32_64(-3));
  printf("%x %d %d %d\n", st16(0x7f00ff), sh8(0x1100), sc_sign(3), uc_sign(1));
  printf("%x %d\n", hi8(0x11223344), then_full(-1));
  printf("%d %d %llx\n", full(1), full_ptr(2), full64(3));
  printf("%x %x\n", narrow_br(0x1000), narrow_br(-0x1000));
  return 0;
}
