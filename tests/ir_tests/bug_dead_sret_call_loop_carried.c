/*
 *  TCC IR - a pure aggregate-return call in a loop feeds the loop's test
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* dead_sret_call (opt/flat/scalar/call_result.c) deleted a pure_via_sret call
 * whose buffer nothing after it read in linear order -- but `b = next(b)` at a
 * loop's bottom is read by the loop's test above it.  At -O1/-Os the loop
 * never advanced.  The scan now covers the whole loop around the call. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct opt { uintptr_t payload; bool is_null; };

static const uintptr_t vals[8] = {0, 3, 0, 0, 7, 9, 0, 1};

static struct opt next_set(uintptr_t start, uintptr_t end)
{
  struct opt r;
  for (uintptr_t i = start; i < end; i++)
    if (vals[i] != 0)
    {
      r.payload = i;
      r.is_null = false;
      return r;
    }
  r.payload = 0;
  r.is_null = true;
  return r;
}

unsigned count_for(uintptr_t end)
{
  unsigned n = 0;
  for (struct opt b = next_set(0, end); !b.is_null; b = next_set(b.payload + 1, end))
    n++;
  return n;
}

uintptr_t sum_while(uintptr_t end)
{
  uintptr_t s = 0;
  struct opt b = next_set(0, end);
  while (!b.is_null)
  {
    s += vals[b.payload];
    b = next_set(b.payload + 1, end);
  }
  return s;
}

/* The buffer read only in an outer loop's test. */
unsigned nested(uintptr_t end)
{
  unsigned rounds = 0;
  struct opt b = next_set(0, end);
  while (!b.is_null)
  {
    for (int k = 0; k < 2; k++)
      rounds++;
    b = next_set(b.payload + 1, end);
  }
  return rounds;
}

/* A call whose result really is dead still may go; the output is the same. */
unsigned dead_in_loop(uintptr_t end)
{
  unsigned n = 0;
  for (uintptr_t i = 0; i < end; i++)
  {
    next_set(i, end);
    n++;
  }
  return n;
}

int main(void)
{
  printf("count %u %u %u\n", count_for(8), count_for(5), count_for(0));
  printf("sum %lu %lu\n", (unsigned long)sum_while(8), (unsigned long)sum_while(2));
  printf("nested %u %u\n", nested(8), nested(1));
  printf("dead %u\n", dead_in_loop(8));
  return 0;
}
