/*
 *  TCC IR - sign extensions of values already sign-extended
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* ra:known_ext tracked only the bits a value may have set, so an i16 copied
 * through struct fields kept `sxth r1,r1; sxth r0,r1`.  It now also tracks the
 * width a value is sign-extended from (SXTB/SXTH, LDRSB/LDRSH, SBFX, AND/OR/
 * XOR/SAR of such values, phis) and turns a re-extension into a copy. */

#include <stdint.h>
#include <stdio.h>

struct one { int16_t f0; };
struct two { void *p; int16_t f1; };

static int16_t poll_mask(void *p, int16_t m)
{
  (void)p;
  return (int16_t)(m & 0x145);
}

/* The Zig C backend's vcall: the i16 argument through two tuple fields. */
__attribute__((noinline)) int16_t vcall(void *a0, int16_t a1)
{
  struct one t0;
  struct two t5;
  t0.f0 = a1;
  t5.p = a0;
  t5.f1 = t0.f0;
  return poll_mask(t5.p, t5.f1);
}

/* Narrowing that changes the value: the extension must stay. */
__attribute__((noinline)) int32_t narrow_sum(int32_t x, int32_t y)
{
  int16_t s = (int16_t)(x + y);
  int8_t c = (int8_t)s;
  return (int32_t)s + (int32_t)c;
}

/* A shift of a sign-extended i16 is still an i16; an i8 cut of it is not. */
__attribute__((noinline)) int32_t sar_cut(int16_t v, int k)
{
  int16_t h = (int16_t)((int32_t)v >> 3);
  int8_t b = (int8_t)h;
  return (int32_t)h * 100 + (int32_t)b + k;
}

/* Both arms extended from 8 bits; the join is read back as an i16. */
__attribute__((noinline)) int32_t join8(int c, int8_t a, int8_t b)
{
  int16_t r;
  if (c)
    r = a;
  else
    r = (int16_t)(b ^ 0x55);
  return (int32_t)(int16_t)r - 7;
}

int main(void)
{
  printf("vcall %d %d %d\n", vcall(0, 0x7fff), vcall(0, -1), vcall(0, 0x0144));
  printf("narrow_sum %d %d %d\n", (int)narrow_sum(0x7fff, 1), (int)narrow_sum(0x12345, 0x80), (int)narrow_sum(-200, 73));
  printf("sar_cut %d %d %d\n", (int)sar_cut(-32768, 1), (int)sar_cut(0x7ff8, 2), (int)sar_cut(-9, 3));
  printf("join8 %d %d %d %d\n", (int)join8(1, -128, 0), (int)join8(0, 0, -128), (int)join8(1, 127, 5), (int)join8(0, 0, 0x2a));
  return 0;
}
