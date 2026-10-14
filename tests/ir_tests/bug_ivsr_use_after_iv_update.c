/*
 * ssa:iv_strength_reduction: the walking pointer started at the IV header value, one step
 * early, when the address reads the IV after its in-loop update.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

static const uint8_t b[24] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24};

__attribute__((noinline)) uint32_t f(const uint8_t *p)
{
  const uint8_t *q = p;
  uint32_t a = 0;
  int i = 2;
  while (1)
  {
    i--;
    a = a * 3 + q[i];
    if (i == 0)
      break;
  }
  return a;
}

__attribute__((noinline)) uint32_t g(const uint8_t *p)
{
  const uint8_t *q = p;
  uint32_t a = 0;
  int i = 20;
  while (1)
  {
    i--;
    a = a * 3 + q[i];
    if (i == 0)
      break;
  }
  return a;
}

int main(void)
{
  printf("%08x\n", (unsigned)f(b));
  printf("%08x\n", (unsigned)g(b));
  return 0;
}
