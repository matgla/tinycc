/*
 *  TCC Tests - float -> double of subnormal floats (RP2350 DCP runtime)
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* The RP2350 DCP's float->double sequence (WXYU/NRDD/RDDG) flushed a
 * subnormal float to zero, so __aeabi_f2d did -- and a tcc running on the
 * board widens every float constant it folds, so it also turned -0x1p-149f
 * into -0.0f.  Subnormal floats are now widened in software. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t fbits(float f)
{
  uint32_t u;
  memcpy(&u, &f, sizeof u);
  return u;
}

static float from_bits(uint32_t u)
{
  float f;
  memcpy(&f, &u, sizeof f);
  return f;
}

static void widen(uint32_t in)
{
  volatile float f = from_bits(in);
  double d = f;
  uint64_t u;
  memcpy(&u, &d, sizeof u);
  printf("%08x -> %08x%08x\n", (unsigned)in, (unsigned)(u >> 32), (unsigned)u);
}

int main(void)
{
  /* the smallest and largest subnormals, both signs, and the normals around them */
  static const uint32_t cases[] = {0x00000001, 0x80000001, 0x00000002, 0x00400000, 0x80400000,
                                   0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x00000000,
                                   0x80000000, 0x3f800000, 0x7f800000};
  for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    widen(cases[i]);
  /* folded by the compiler: negation goes through a widen and a narrow */
  printf("folded %08x %08x %08x\n", (unsigned)fbits(-0x1p-149f), (unsigned)fbits(-0x0.000002p-126f),
         (unsigned)fbits(-0x1p-127f));
  return 0;
}
