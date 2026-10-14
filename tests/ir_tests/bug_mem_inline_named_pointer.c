/*
 *  TCC IR - Named pointer values in small memory copies
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

__attribute__((noinline)) static void *locate(void *base, unsigned index)
{
  return (char *)base + 1 + index * 4;
}

__attribute__((noinline)) static void replay(void *base, const uint32_t *entries, unsigned count)
{
  unsigned index = 0;
  void *destination;
  uint32_t value;
loop:
  if (index >= count)
    return;
  destination = locate(base, index);
  value = entries[index] + 3;
  memcpy(destination, &value, sizeof(value));
  ++index;
  goto loop;
}

__attribute__((noinline)) static uint32_t decode(const void *base, unsigned offset)
{
  const char *source;
  uint32_t result;
  source = (const char *)base + offset;
  memcpy(&result, source, sizeof(result));
  return result;
}

__attribute__((noinline)) static void redirect(char **pointer, char *target)
{
  *pointer = target;
}

__attribute__((noinline)) static void fill(char *base)
{
  char *destination = base + 1;
  redirect(&destination, base + 5);
  memset(destination, 0x5a, 4);
}

__attribute__((noinline)) static void *copy_result(void *base, uint32_t value)
{
  char *destination = (char *)base + 1;
  return memcpy(destination, &value, 4);
}

int main(void)
{
  unsigned char buffer[18];
  const uint32_t entries[] = { 1, 0x01020304, 0xfffffffe, 0x11223344 };
  memset(buffer, 0xa5, sizeof(buffer));
  replay(buffer, entries, 4);
  for (unsigned i = 0; i < 4; ++i)
    if (decode(buffer, 1 + i * 4) != entries[i] + 3)
      return 1;
  if (buffer[0] != 0xa5 || buffer[17] != 0xa5)
    return 2;
  fill((char *)buffer);
  if (decode(buffer, 5) != 0x5a5a5a5a || decode(buffer, 1) != 4)
    return 3;
  if (copy_result(buffer, 0xdeadbeef) != buffer + 1 || decode(buffer, 1) != 0xdeadbeef)
    return 4;
  puts("named-pointer copies ok");
  return 0;
}
