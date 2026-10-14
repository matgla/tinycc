/*
 *  TCC IR - Shared scaled address regression
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <string.h>
#include "asm/shifted_address_shared_use.c"

const char strings[][16] = {"first", "second", "third", "fourth", "123456789"};
static unsigned failures;

#define CHECK_STRING(expr, expected) do { \
  const char *s = (expr); \
  unsigned n = strlen(s); \
  if (n != (expected)) { \
    printf("wrong length %u: %s\n", n, s); \
    ++failures; \
  } \
} while (0)

static void check_strings(void)
{
  int i = 4;
  CHECK_STRING(strings[i], 9);
  CHECK_STRING(&strings[i][0], 9);
  CHECK_STRING(&strings[i][1], 8);
  CHECK_STRING(&strings[i][4], 5);
}

__attribute__((noinline)) static int operand(int j, int which)
{
  return 100 * j + which;
}

__attribute__((noinline)) static void build_cache(int *cache, int n, const unsigned *ops)
{
  for (int j = 0; j < n; j++) {
    int *slot = &cache[(unsigned)j * 3];
    slot[0] = slot[1] = slot[2] = (-2147483647 - 1);
    if (!ops[j])
      continue;
    if (ops[j] & 1)
      slot[0] = operand(j, 0);
    if (ops[j] & 2)
      slot[1] = operand(j, 1);
    if (ops[j] & 4)
      slot[2] = operand(j, 2);
  }
}

static int check_cache(void)
{
  unsigned ops[] = {7, 0, 3, 4, 5};
  int cache[15];
  build_cache(cache, 5, ops);
  for (unsigned j = 0; j < 5; j++)
    for (unsigned k = 0; k < 3; k++)
      if (cache[3 * j + k] != ((ops[j] & (1u << k)) ? operand(j, k) : (-2147483647 - 1)))
        return 0;
  return 1;
}

int main(void)
{
  struct E list[] = {{0, 3}, {8, 5}, {15, 9}, {1001, 10}, {40, 1}};
  unsigned expected[] = {0, 3, 16, 22, 3, 3};
  for (unsigned n = 0; n <= 5; n++) {
    if (shared(list, n, 0) != expected[n])
      return 1;
  }
  if (single(list, 5) != 1064)
    return 2;
  list[3].at = 1000;
  if (shared(list, 5, 0) != 1057)
    return 3;
  check_strings();
  if (failures)
    return 4;
  if (!check_cache())
    return 5;
  puts("PASS");
  return 0;
}
