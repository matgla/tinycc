/*
 *  TCC Tests - Short-active spill swaps under replay-loop pressure
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

struct Entry { uint32_t at, value; };
struct Slice { const struct Entry *ptr; uintptr_t len; };

__attribute__((noinline))
uintptr_t replay_range(struct Slice list, const uintptr_t *bases,
                       uint8_t *data, uint32_t data_words, uintptr_t start,
                       uintptr_t p0, uintptr_t p1, uintptr_t p2, uintptr_t p3,
                       uintptr_t p4, uintptr_t p5, uintptr_t p6, uintptr_t p7,
                       uintptr_t p8, uintptr_t p9)
{
  uintptr_t a = p0 ^ p1, b = p2 + p3, d = p5 | p6;
  uintptr_t e = p7 & p8, f = p9 ^ p0, g = p1 + p9, h = p2 ^ p8;
  (void)p4;
  for (uintptr_t i = start; i < list.len; i++) {
    struct Entry en = list.ptr[i];
    uint32_t index = en.at >> 8;
    if (index >= data_words) return i + a;
    uint32_t v = en.value + (uint32_t)bases[en.at & 255];
    memcpy(data + index * 4, &v, sizeof(v));
  }
  return list.len + b + d + e + f + g + h + a;
}

int main(void)
{
  const struct Entry entries[] = {{0, 11}, {513, 22}, {1024, 33}, {1537, 44}};
  const uintptr_t bases[] = {100, 200};
  const struct Slice list = {entries, 4};
  const unsigned starts[] = {0, 0, 2, 0, 4};
  const unsigned bounds[] = {8, 3, 8, 0, 8};
  const uintptr_t results[] = {62, 5, 62, 3, 62};
  for (int c = 0; c < 5; c++) {
    uint32_t data[8] = {0};
    uintptr_t result = replay_range(list, bases, (uint8_t *)data, bounds[c], starts[c],
                                    1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    if (result != results[c]) return 1;
    for (unsigned j = 0; j < 8; j++) {
      uint32_t expected = 0;
      if (j % 2 == 0 && j / 2 >= starts[c] && j < bounds[c])
        expected = entries[j / 2].value + bases[entries[j / 2].at & 255];
      if (data[j] != expected) return 2;
    }
  }
  puts("ok");
  return 0;
}
