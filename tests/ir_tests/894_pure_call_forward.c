/*
 *  TCC - Read-only wrapper and call-reuse regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* TCC Tests - Forward call reuse keeps memory, control and argument barriers */
#include <stdio.h>
#include "asm/pure_call_forward.c"

int main(void)
{
  unsigned char p[48], q[48] = {0, 0, 0, 0, 'q', 'q', 0};
  for (usize n = 0; n < 20; n++) {
    for (usize mode = 0; mode < 2; mode++) {
      for (usize i = 0; i < sizeof p; i++) p[i] = 0;
      for (usize i = 0; i < n; i++) p[4 + i] = 'a';
      usize x = mode ? ((mode * 3u) ^ 17u) : mode + 7u;
      if (clean_diamond(p, mode) != n * 100u + n + x) return 1;
      if (changed_pointer(p, mode) != n * 100u + n - (mode && n != 0)) return 2;
      if (skipped_first(p, mode) != (mode ? n : 7u) * 100u + n) return 3;
      if (external_entry(p, mode) != (mode ? 7u : n) * 100u + n) return 4;
      if (changed_base(p, q, mode) != n * 100u + (mode ? 2u : n)) return 5;
      if (asm_barrier(p) != n * 100u + n) return 6;
      observed = 0;
      if (conditional_volatile(p, mode) != n * 100u + n || observed != mode) return 7;
      if (conditional_store(p, mode) != n * 100u + n + mode) return 8;
      p[4 + n] = 0;
      if (conditional_call(p, mode) != n * 100u + n + mode) return 9;
      p[4 + n] = 0;
      if (loop_store(p, 7) != n * 100u + 7u) return 10;
    }
  }
  puts("forward call reuse ok");
  return 0;
}
