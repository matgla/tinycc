/*
 *  TCC IR - a store through a pointer of the value just loaded from there
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Zig's bit_set.setRangeValue expanded with value == false ends in
 * `w = masks[i]; masks[i] = w | 0`: a store that changes nothing.
 * ssa:ptr_silent_store (opt/ssa/memory/ptr_store_dse.c) drops it, and the
 * reloads feeding it die.  A write in between, another path into the store,
 * or a redefined address keeps it. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct bitset { uint32_t bit_length; uint32_t *masks; };

static void set_range(struct bitset *const a0, uint32_t start, uint32_t end, bool const value)
{
  struct bitset *const *t1;
  struct bitset *t6;
  uint32_t *t19, *t7;
  uint32_t t5, t8, t13, t20, t15, t16;
  uint8_t t12, t14;
  struct bitset *t0 = a0;
  t1 = &t0;
  t5 = start >> 5;
  t12 = (uint8_t)(start & 31);
  t14 = (uint8_t)(end & 31);
  t8 = end >> 5;
  if (t5 == t8)
  {
    t15 = 0xffffffffu << t12;
    t16 = 0xffffffffu >> (31 - (uint8_t)(t14 - 1));
    t6 = *t1;
    t19 = t6->masks;
    t7 = &t19[t5];
    t8 = *t7;
    t13 = t15;
    t20 = t13 & t16;
    t20 = ~t20;
    t20 = t8 & t20;
    *t7 = t20;
    t20 = 0u - (uint32_t)value;
    t15 = t20 << t12;
    t16 = (0u - (uint32_t)value) >> (31 - (uint8_t)(t14 - 1));
    t6 = *t1;
    t19 = t6->masks;
    t7 = &t19[t5];
    t5 = *t7;
    t8 = t15 & t16;
    t8 = t5 | t8;
    *t7 = t8;
  }
}

__attribute__((noinline)) void clear_range(struct bitset *b, uint32_t s, uint32_t e) { set_range(b, s, e, false); }
__attribute__((noinline)) void fill_range(struct bitset *b, uint32_t s, uint32_t e) { set_range(b, s, e, true); }

/* A write through another pointer in between: q may be p. */
__attribute__((noinline)) void restore_after(uint32_t *p, uint32_t *q, uint32_t v)
{
  uint32_t w = *p;
  *q = v;
  *p = w;
}

/* A path that writes first joins at the store. */
__attribute__((noinline)) void restore_join(uint32_t *p, int c)
{
  uint32_t w = *p;
  if (c)
  {
    *p = 99;
    goto put;
  }
  w = w + 0;
put:
  *p = w;
}

/* The index changes between the load and the store. */
__attribute__((noinline)) void shift_copy(uint32_t *p, uint32_t i)
{
  uint32_t w = p[i];
  i = i + 1;
  p[i] = w;
}

/* A call in between may write *p. */
static uint32_t *global_p;
__attribute__((noinline)) static void poke(void) { *global_p = 7; }
__attribute__((noinline)) void restore_call(uint32_t *p)
{
  uint32_t w = *p;
  global_p = p;
  poke();
  *p = w;
}

/* Narrow access: the byte loaded is the byte stored. */
__attribute__((noinline)) void byte_back(uint8_t *p, uint32_t i)
{
  uint8_t w = p[i];
  p[i] = w | 0;
}

int main(void)
{
  uint32_t words[2] = {0xffffffffu, 0x0f0f0f0fu};
  struct bitset b = {64, words};
  clear_range(&b, 4, 12);
  printf("clear %08lx %08lx\n", (unsigned long)words[0], (unsigned long)words[1]);
  fill_range(&b, 36, 44);
  printf("fill %08lx %08lx\n", (unsigned long)words[0], (unsigned long)words[1]);
  clear_range(&b, 32, 40);
  printf("clear2 %08lx %08lx\n", (unsigned long)words[0], (unsigned long)words[1]);

  uint32_t x = 5, y = 6;
  restore_after(&x, &x, 8);
  restore_after(&x, &y, 9);
  printf("after %lu %lu\n", (unsigned long)x, (unsigned long)y);
  restore_join(&x, 1);
  printf("join %lu\n", (unsigned long)x);
  restore_join(&x, 0);
  printf("join0 %lu\n", (unsigned long)x);
  uint32_t arr[3] = {1, 2, 3};
  shift_copy(arr, 0);
  printf("shift %lu %lu %lu\n", (unsigned long)arr[0], (unsigned long)arr[1], (unsigned long)arr[2]);
  restore_call(&y);
  printf("call %lu\n", (unsigned long)y);
  uint8_t bytes[2] = {0x5a, 0xa5};
  byte_back(bytes, 1);
  printf("byte %02x %02x\n", bytes[0], bytes[1]);
  return 0;
}
