/*
 * ssa:dead_loop: an exit phi was replaced by the latch constant although a mid-body break
 * reaches the same exit block earlier with the preheader value.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

int8_t S[8] = {1, 1, 1, 1, 1, 1, -1, 1};
int8_t T[8] = {1, 1, 1, 1, 1, 1, 1, 1};
int8_t U[8] = {1, 1, -1, 1, 1, 1, 1, 1};

/* Each helper is called once on a global so it inlines with a constant base. */
#define DOWN(name)                  \
  static uint32_t name(const int8_t *s)  \
  {                                      \
    uint32_t c = 0xffff;                 \
    int i;                               \
    for (i = 6; i >= 0; i -= 1)          \
    {                                    \
      if (s[i] < 0)                  \
        break;                           \
      c = 0x8000;                        \
    }                                    \
    return c;                            \
  }

#define UP(name)                    \
  static uint32_t name(const int8_t *s, uint32_t x) \
  {                                      \
    uint32_t c = 0xffff, k = 0;          \
    int i;                               \
    for (i = 0; i < 5; i += 2)           \
    {                                    \
      if (s[i] < 0)                  \
        break;                           \
      if (s[i] < 0)                  \
        break;                           \
      c = x;                             \
      k++;                               \
    }                                    \
    return c + k;                        \
  }

DOWN(down_break)
DOWN(down_full)
UP(up_full)
UP(up_break)

int main(void)
{
  printf("%08x\n", (unsigned)down_break(S));
  printf("%08x\n", (unsigned)down_full(T));
  printf("%08x\n", (unsigned)up_full(T, 7));
  printf("%08x\n", (unsigned)up_break(U, 7));
  return 0;
}
