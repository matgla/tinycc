/*
 *  TCC IR - Shared scaled address assembly regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

typedef unsigned u32;

struct E {
  u32 at;
  u32 value;
};

u32 shared(const struct E *list, u32 n, u32 *out) {
  u32 t = 0;
  for (u32 i = 0; i < n; i++) {
    const struct E *e = &list[i];
    if (e->at > 1000)
      return i;
    t += e->at ^ e->value;
  }
  return t;
}

u32 single(const struct E *list, u32 n) {
  u32 t = 0;
  for (u32 i = 0; i < n; i++)
    t += list[i].at;
  return t;
}
