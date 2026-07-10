/*
 *  Regression test for folding signed 64-bit constant comparisons.
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>

#define TRUE_VALUE 13
#define FALSE_VALUE 140

int flt(x)
long long int x;
{
  return x < 0 ? TRUE_VALUE : FALSE_VALUE;
}

int main(void)
{
  if (flt(0x8000000000000000LL) != TRUE_VALUE)
  {
    printf("FAIL hex-min\n");
    return 1;
  }

  if (flt(-9223372036854775807LL - 1LL) != TRUE_VALUE)
  {
    printf("FAIL expr-min\n");
    return 1;
  }

  if (flt(0LL) != FALSE_VALUE)
  {
    printf("FAIL zero\n");
    return 1;
  }

  printf("PASS\n");
  return 0;
}
