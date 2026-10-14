/*
 *  TCC IR - Aggregate copy alignment regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>

struct words { unsigned x[4]; };
struct __attribute__((packed)) holder { unsigned char tag; struct words value; };

__attribute__((noinline)) void copy_packed(struct holder *d, const struct holder *s)
{
  d->value = s->value;
}

__attribute__((noinline)) void load_packed(struct words *d, const struct holder *s)
{
  *d = s->value;
}

__attribute__((noinline)) void store_packed(struct holder *d, const struct words *s)
{
  d->value = *s;
}

int main(void)
{
  _Alignas(8) unsigned char src[32], dst[32];
  const struct words expected = {{0x12345678u, 0x90abcdefu, 0x11223344u, 0xfedcba98u}};
  for (unsigned a = 0; a < 4; a++)
    for (unsigned b = 0; b < 4; b++)
    {
      memset(src, 0xa5, sizeof src);
      memset(dst, 0x5a, sizeof dst);
      struct holder *s = (struct holder *)(src + a);
      struct holder *d = (struct holder *)(dst + b);
      memcpy(src + a + 1, &expected, sizeof expected);
      copy_packed(d, s);
      if (memcmp(dst + b + 1, &expected, sizeof expected) || dst[b] != 0x5a || dst[b + 17] != 0x5a)
        return 1;
      struct words actual;
      load_packed(&actual, s);
      if (memcmp(&actual, &expected, sizeof expected))
        return 2;
      memset(dst, 0x5a, sizeof dst);
      store_packed(d, &actual);
      if (memcmp(dst + b + 1, &expected, sizeof expected) || dst[b] != 0x5a || dst[b + 17] != 0x5a)
        return 3;
    }
  puts("OK");
  return 0;
}
