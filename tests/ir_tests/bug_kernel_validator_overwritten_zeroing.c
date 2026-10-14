/*
 *  TCC IR - ELF validator overwritten local initialization regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>

typedef unsigned long long U64;

__attribute__((noinline)) unsigned sum_records(const U64 *p, unsigned n)
{
  unsigned sum = 0;
  for (unsigned i = 0; i < n; i++)
  {
    U64 value = p[i], local;
    memset(&local, 0, sizeof(local));
    local = value;
    const U64 *q = &local;
    sum += (unsigned)*q;
    sum += (unsigned)(*q >> 32);
  }
  return sum;
}

__attribute__((noinline)) U64 observe(const U64 *p)
{
  return *p;
}

__attribute__((noinline)) U64 read_before_write(U64 value)
{
  U64 local;
  memset(&local, 0, sizeof(local));
  U64 before = observe(&local);
  local = value;
  return before + local;
}

__attribute__((noinline)) U64 partial_write(unsigned value)
{
  U64 local;
  memset(&local, 0, sizeof(local));
  memcpy(&local, &value, sizeof(value));
  return local;
}

int main(void)
{
  const U64 records[] = {0x0000000200000001ULL, 0x0000000400000003ULL, 0x0000000600000005ULL};
  unsigned sum = sum_records(records, 3);
  unsigned empty = sum_records(records, 0);
  U64 read = read_before_write(0x123456789abcdef0ULL);
  U64 partial = partial_write(0xabcdef01U);
  printf("sum=%u empty=%u read=%llx partial=%llx\n", sum, empty, read, partial);
  return sum != 21 || empty != 0 || read != 0x123456789abcdef0ULL || partial != 0xabcdef01ULL;
}
