/*
 * ssa:loop_unroll: compute_trip_count treated a back edge taken while i == C as the loop
 * that runs until i == C, unrolling a one/two trip loop to C - init copies.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) uint32_t f(uint32_t acc)
{
  int i = 0;
  do
  {
    acc = acc * 3 + 1;
    i++;
  } while (i == 3);
  return acc;
}

__attribute__((noinline)) uint32_t g(uint32_t acc)
{
  int i = 2;
  do
  {
    acc = acc * 3 + 1;
    i++;
  } while (i == 3);
  return acc;
}

/* The real until-equal shape must keep its trip count. */
__attribute__((noinline)) uint32_t h(uint32_t acc)
{
  for (int i = 0; i != 4; i++)
    acc = acc * 3 + 1;
  return acc;
}

int main(void)
{
  printf("%08x\n", (unsigned)f(0));
  printf("%08x\n", (unsigned)g(0));
  printf("%08x\n", (unsigned)h(0));
  return 0;
}
