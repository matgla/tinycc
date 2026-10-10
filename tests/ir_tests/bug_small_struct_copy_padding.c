/*
 *  TCC - Small aggregate copies with partial padding words
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <stdint.h>
#include <stdio.h>

typedef struct { uint32_t payload; uint8_t flag; } ByteTail;
typedef struct { uint32_t payload; uint16_t flag; } HalfTail;
typedef union { ByteTail narrow; uint32_t words[2]; } WholeWord;
typedef struct { ByteTail inner; uint32_t suffix; } Nested;

__attribute__((noinline)) void copy_byte(ByteTail *d, const ByteTail *s) { *d = *s; }
__attribute__((noinline)) void copy_half(HalfTail *d, const HalfTail *s) { *d = *s; }
__attribute__((noinline)) void copy_union(WholeWord *d, const WholeWord *s) { *d = *s; }
__attribute__((noinline)) void copy_nested(Nested *d, const Nested *s) { *d = *s; }

__attribute__((noinline)) unsigned half_after_copy(unsigned flag)
{
  HalfTail result, copy;
  __asm__ volatile("" ::: "memory");
  result.payload = 0;
  result.flag = flag;
  copy = result;
  return copy.flag;
}

__attribute__((noinline)) void acquire(uint32_t *state)
{
  ByteTail result, copy;
retry:
  result.payload = 0;
  result.flag = __atomic_compare_exchange_n(state, &result.payload, 1, 1,
                                             2, 0);
  copy = result;
  if (!copy.flag)
  {
    __asm__ volatile("" ::: "memory");
    goto retry;
  }
}

int main(void)
{
  for (uint32_t v = 0; v < 256; v++)
  {
    ByteTail a = {v * 0x1020304u, (uint8_t)v}, b;
    HalfTail h = {v ^ 0xabcdef01u, (uint16_t)(v * 257)}, j;
    WholeWord u, w;
    Nested n = {{v * 17, (uint8_t)(v ^ 0x55)}, v * 31}, m;
    u.words[0] = v;
    u.words[1] = v * 0x01010101u;
    copy_byte(&b, &a);
    copy_byte(&b, &b);
    copy_half(&j, &h);
    copy_union(&w, &u);
    copy_nested(&m, &n);
    if (b.payload != a.payload || b.flag != a.flag ||
        j.payload != h.payload || j.flag != h.flag ||
        w.words[0] != u.words[0] || w.words[1] != u.words[1] ||
        m.inner.payload != n.inner.payload || m.inner.flag != n.inner.flag || m.suffix != n.suffix ||
        half_after_copy(v * 0x01010101u) != (uint16_t)(v * 0x01010101u))
      return 1;
    uint32_t state = 0;
    acquire(&state);
    if (state != 1)
      return 2;
  }
  puts("OK");
  return 0;
}
