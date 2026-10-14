/*
 *  TCC IR - 0/1 select of a known-boolean value regression
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* Zig's ComponentIterator.init: `root_len = if (len > 0 and path[0] == '/') 1
 * else 0` re-tested the 0/1 join and selected 1/0 again (cmp; ite ne; movne;
 * moveq).  ra:known_ext now turns `SELECT #1,#0` on a {0,1} TEMP's test into
 * a copy (`#0,#1` into `x ^ 1`); a value that can be 2 keeps its select. */
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

/* The Zig C-backend shape: a bool diamond, then `if (t7) t2 = 1; else t2 = 0`. */
__attribute__((noinline)) uintptr_t root_len(const uint8_t *p, uintptr_t n)
{
  bool t6, t7;
  uintptr_t t2;
  t6 = n > 0;
  if (t6) {
    t6 = p[0] == '/';
    t7 = t6;
    goto b1;
  }
  t7 = false;
b1:
  if (t7) {
    t2 = 1;
    goto b0;
  }
  t2 = 0;
b0:
  return t2;
}

/* The complement: `if (t7) t2 = 0; else t2 = 1`. */
__attribute__((noinline)) uintptr_t not_root(const uint8_t *p, uintptr_t n)
{
  bool t6, t7;
  uintptr_t t2;
  t6 = n > 0;
  if (t6) {
    t6 = p[0] == '/';
    t7 = t6;
    goto b1;
  }
  t7 = false;
b1:
  if (t7) {
    t2 = 0;
    goto b0;
  }
  t2 = 1;
b0:
  return t2;
}

/* One arm of the join is 2: the select is not a copy. */
__attribute__((noinline)) uintptr_t two_arm(const uint8_t *p, uintptr_t n)
{
  unsigned t7;
  uintptr_t t2;
  if (n > 0)
    t7 = p[0] == '/';
  else
    t7 = 2;
  if (t7)
    t2 = 1;
  else
    t2 = 0;
  return t2 * 10 + t7;
}

/* A select of other constants on a 0/1 value stays a select. */
__attribute__((noinline)) uintptr_t scaled(const uint8_t *p, uintptr_t n)
{
  bool t7 = n > 0 && p[0] == '/';
  return t7 ? 5u : 0u;
}

/* The 0/1 value used after the select too, and the flags read again. */
__attribute__((noinline)) uintptr_t reused(const uint8_t *p, uintptr_t n, uintptr_t k)
{
  bool t7 = n > 1 && p[1] == 'a';
  uintptr_t t2 = t7 ? 1u : 0u;
  if (t7)
    k += 100;
  return t2 + k + (uintptr_t)t7 * 1000;
}

/* The 0/1 join narrowed before its test (`uxtb`, a UBFX #0,#8 in the IR):
 * the extract keeps only the bits its operand can have, so still 0/1. */
__attribute__((noinline)) uintptr_t narrowed(const uint8_t *p, uintptr_t n, uintptr_t k)
{
  unsigned t7 = n > 1 && p[1] == 'a';
  uintptr_t t2 = (uint8_t)t7 ? 1u : 0u;
  if (t7)
    k += 100;
  return t2 + k + (uintptr_t)t7 * 1000;
}

/* A field extracted from {0x100, 0x200} is {1, 2}: the select stays. */
__attribute__((noinline)) uintptr_t field(const uint8_t *p, uintptr_t n)
{
  unsigned t9 = (n > 0 && p[0] == '/') ? 0x100u : 0x200u;
  unsigned f = (t9 >> 8) & 3u;
  return (f ? 7u : 0u) + f * 10u;
}

static const char *const paths[] = {"/usr/lib", "usr", "", "/", "xa", "/a", "aa"};

int main(void)
{
  for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    const uint8_t *p = (const uint8_t *)paths[i];
    uintptr_t n = 0;
    while (p[n])
      n++;
    printf("'%s': root=%lu not=%lu two=%lu scaled=%lu reused=%lu narrowed=%lu field=%lu\n", paths[i],
           (unsigned long)root_len(p, n), (unsigned long)not_root(p, n), (unsigned long)two_arm(p, n),
           (unsigned long)scaled(p, n), (unsigned long)reused(p, n, i), (unsigned long)narrowed(p, n, i),
           (unsigned long)field(p, n));
  }
  return 0;
}
