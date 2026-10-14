/*
 *  TCC Tests - a by-value struct copy whose destination address escapes
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/* The escape_copy_call pass folds the frontend's word-chunked copy of a
 * by-value struct parameter into one __aeabi_memmove4 call when the local
 * copy's address escapes into a callee (so forwarding cannot delete it).
 * Correctness: the callee reads the copy while the parameter is re-read
 * afterwards, and the copy must not alias the parameter home. */

#include <stdint.h>
#include <stdio.h>

struct Cci { int32_t (*apply)(void *ctx, int32_t v); void *ctx; int32_t mem[3]; int32_t *ref; };

__attribute__((noinline))
static int32_t sum3(void *ctx, int32_t v)
{
  struct Cci *c = (struct Cci *)ctx;
  return v + c->mem[0] + c->mem[1] + c->mem[2];
}

__attribute__((noinline))
static int32_t sum1(void *ctx, int32_t v)
{
  struct Cci *c = (struct Cci *)ctx;
  return v + c->mem[1];
}

__attribute__((noinline))
int32_t through_escaping_copy(struct Cci dev, int32_t seed, int32_t extra)
{
  struct Cci local = dev;
  /* The address of the copy escapes: apply receives every word of it. */
  int32_t r = local.apply(&local, seed);
  /* The parameter is re-read after the call: a copy aliased onto the
   * parameter home would see whatever the callee wrote through it. */
  local.mem[1] = -local.mem[1];
  return r + dev.mem[1] + dev.mem[2] - 30 + extra;
}

/* A second escaping-copy shape in a loop, so the rewrite has to hold across
 * backedges and call sites. */
__attribute__((noinline))
int32_t copies_in_loop(const struct Cci *src, int32_t n)
{
  int32_t acc = 0;
  for (int32_t i = 0; i < n; i++) {
    struct Cci local = *src;
    local.mem[2] += i;
    acc += local.apply(&local, 1);
  }
  return acc;
}

int main(void)
{
  static int32_t cell = 7;
  struct Cci dev = {.apply = sum3, .ctx = 0, .mem = {10, 20, 30}, .ref = &cell};
  struct Cci dev2 = {.apply = sum1, .ctx = 0, .mem = {1, 2, 3}, .ref = &cell};

  if (through_escaping_copy(dev, 5, 1000) != 5 + 60 + 20 + 30 - 30 + 1000)
    return 1;
  if (dev.mem[1] != 20 || dev.mem[0] != 10)
    return 2;
  if (through_escaping_copy(dev2, 100, 0) != 100 + 2 + 2 + 3 - 30)
    return 3;

  struct Cci loop_src = {.apply = sum3, .ctx = 0, .mem = {7, 8, 9}, .ref = &cell};
  /* 3 turns: (1+7+8+9) + (1+7+8+10) + (1+7+8+11) */
  if (copies_in_loop(&loop_src, 3) != 25 + 26 + 27)
    return 4;
  if (loop_src.mem[0] != 7 || loop_src.mem[2] != 9)
    return 5;

  puts("ok");
  return 0;
}
