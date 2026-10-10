/*
 *  TCC Regression - flag-neutral windows between a compare and its branch
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

/* The setif fuse family (setif_branch_fuse / setif_paired_fuse /
 * setif_window_fuse / setif_branch_remat) branches on the compare's own
 * flags whenever a materialized 0/1 bool is only ever zero-tested.  The
 * window variant legalized instructions between the SETIF and its test
 * (stores of unrelated values, copy chains, flag-neutral ALU) and the
 * remat variant grew two-register compares.  Executing every shape pins
 * that each branch still tests the value its compare produced -- not
 * flags somebody in between left behind (kernel-ls-bin-scan-family-gap,
 * mem.eql / findScalarPos shapes). */

#include <stdint.h>
#include <stdio.h>

struct pair { uintptr_t len; const uint8_t *ptr; };
struct pos { int64_t p; int ok; };

static struct pair gp = { 8, (const uint8_t *)"12345678" };

/* bool as value AND branch, with unrelated stores + ALU landing between
 * the compare and the branch (the bounds-check quartets of mem.eqlBytes
 * carry the struct-copy stores there). */
__attribute__((noinline))
static int window_shape(uintptr_t n, struct pair *out)
{
  struct pair p = gp;
  int t = p.len < n;
  out->len = p.len + 2;
  out->ptr = p.ptr;
  if (t)
    return (int)(n - p.len);
  return -(int)p.len;
}

/* copy chain feeding the branch; the bool is also returned as a value. */
__attribute__((noinline))
static int chain_shape(int a, int b)
{
  int t = a < b;
  int u = t;
  int v = u;
  return (v ? 100 : 200) + t;
}

/* one two-register compare, its bool branched on twice (remat shape). */
__attribute__((noinline))
static int twouse_shape(int a, int b)
{
  int t = a < b;
  if (t)
    return 10;
  if (t)
    return 20;
  return 30;
}

/* narrow (u8) stack-passed param read inside a loop -- the
 * findScalarPos needle: ra:stack_param_promote caches it in a register. */
__attribute__((noinline))
static void find_scalar(struct pos *out, const uint8_t *buf, uint64_t len,
                        uint64_t start, uint8_t needle)
{
  for (uint64_t i = start; i < len; i++)
    if (buf[i] == needle) {
      out->p = (int64_t)i;
      out->ok = 1;
      return;
    }
  out->p = -1;
  out->ok = 0;
}

int main(void)
{
  struct pair o = { 0, 0 };
  if (window_shape(3, &o) != -8 || o.len != 10 || o.ptr != gp.ptr)
    return 1;
  if (window_shape(11, &o) != 3)
    return 2;
  if (chain_shape(1, 2) != 101 || chain_shape(2, 1) != 200)
    return 3;
  if (twouse_shape(1, 2) != 10 || twouse_shape(2, 1) != 30)
    return 4;

  static const uint8_t buf[] = "abcabcabc";
  struct pos p;
  find_scalar(&p, buf, 9, 0, 'c');
  if (p.ok != 1 || p.p != 2)
    return 5;
  find_scalar(&p, buf, 9, 3, 'c');
  if (p.ok != 1 || p.p != 5)
    return 6;
  find_scalar(&p, buf, 9, 0, 'z');
  if (p.ok != 0 || p.p != -1)
    return 7;

  printf("window %d %d\n", window_shape(20, &o), o.len);
  printf("chain %d %d\n", chain_shape(1, 2), chain_shape(3, 2));
  printf("twouse %d %d\n", twouse_shape(1, 2), twouse_shape(5, 4));
  printf("needle %d %d %d\n", (int)p.p, p.ok, buf[2]);
  return 0;
}
