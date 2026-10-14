/*
 * ssa:first_iter_exit: a loop whose mid-body exit is taken on the first trip was deleted
 * together with the part of the body that runs before the exit test.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) uint64_t f(const uint8_t *p, uint32_t k)
{
  uint32_t acc = k;
  int i = 0;
  uint32_t cnt = 0;
  for (;;)
  {
    acc = acc * 31 + p[i];
    cnt++;
    if (i == 0)
      break;
    acc ^= p[i];
    i++;
  }
  return (uint64_t)acc ^ ((uint64_t)cnt << 32);
}

static const uint8_t b[4] = {0x85, 1, 2, 3};

int main(void)
{
  uint64_t r = f(b, 0x12345678);
  printf("%08x %08x\n", (unsigned)r, (unsigned)(r >> 32));
  return 0;
}
