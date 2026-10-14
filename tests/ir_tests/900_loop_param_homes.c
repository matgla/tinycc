/*
 *  TCC Regression - Loop-invariant parameter homes across calls
 *
 *  Copyright (c) 2026 Mateusz Stadnik
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
struct Code { const uint8_t *ptr; uintptr_t len; };

__attribute__((noinline)) void copy_entry(void *dest, const struct Entry *entry)
{
  memcpy(dest, entry, sizeof *entry);
}

__attribute__((noinline))
void thunk_replay(struct Slice list, struct Code code, uint8_t *dest, uintptr_t base)
{
  for (uintptr_t i = 0; i < list.len; i++) {
    struct Entry e = list.ptr[i];
    uintptr_t off = base + (e.at >> 8) * 4;
    if (off % code.len == 8)
      copy_entry(dest + i * 8, &e);
  }
}

__attribute__((noinline)) void change_len(struct Slice *list)
{
  --list->len;
}

__attribute__((noinline)) unsigned escaped_home(struct Slice list)
{
  unsigned sum = 0;
  for (unsigned i = 0; i < list.len; ++i) {
    sum += list.ptr[i].value;
    change_len(&list);
  }
  return sum + list.len;
}

__attribute__((noinline)) unsigned written_home(struct Slice list)
{
  unsigned sum = 0;
  for (unsigned i = 0; i < list.len; ++i) {
    sum += list.ptr[i].value;
    --list.len;
  }
  return sum + list.len;
}

__attribute__((noinline)) unsigned pressured_home(struct Slice list, unsigned *out,
                                                 unsigned a, unsigned b, unsigned c, unsigned d,
                                                 unsigned e, unsigned f, unsigned g, unsigned h)
{
  for (unsigned i = 0; i < list.len; ++i) {
    copy_entry(out, &list.ptr[i]);
    a += out[0]; b ^= out[1]; c += a; d ^= b;
    e += c; f ^= d; g += e; h ^= f;
  }
  return a + b + c + d + e + f + g + h;
}

int main(void)
{
  const struct Entry entries[] = {{0, 11}, {256, 22}, {512, 33}, {768, 44}};
  struct Entry out[4] = {{99, 99}, {99, 99}, {99, 99}, {99, 99}};
  thunk_replay((struct Slice){entries, 0}, (struct Code){0, 0}, (uint8_t *)out, 0);
  printf("zero %u %u\n", out[0].at, out[0].value);
  thunk_replay((struct Slice){entries, 4}, (struct Code){0, 16}, (uint8_t *)out, 4);
  for (unsigned i = 0; i < 4; ++i)
    printf("%u %u\n", out[i].at, out[i].value);
  printf("changed %u %u\n", escaped_home((struct Slice){entries, 4}),
         written_home((struct Slice){entries, 4}));
  unsigned words[2];
  printf("pressure %u\n", pressured_home((struct Slice){entries, 4}, words, 1, 2, 3, 4, 5, 6, 7, 8));
  return 0;
}
