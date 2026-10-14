/*
 *  TCC IR - Inline a copy to a named pointer value
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

extern void *memcpy(void *, const void *, unsigned);
extern void *locate(void *, unsigned);

void replay(void *base, const unsigned *entries, unsigned count)
{
  unsigned index = 0;
  void *destination;
  unsigned value;
loop:
  if (index >= count)
    return;
  destination = locate(base, index);
  value = entries[index] + 3;
  memcpy(destination, &value, 4);
  ++index;
  goto loop;
}
