/*
 *  TCC IR - Inline a copy from a named pointer value
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

extern void *memcpy(void *, const void *, unsigned);

unsigned decode(const void *base, unsigned offset)
{
  const char *source;
  unsigned result;
  source = (const char *)base + offset;
  memcpy(&result, source, 4);
  return result;
}
