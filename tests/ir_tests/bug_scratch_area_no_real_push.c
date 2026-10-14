/*
 *  TCC IR - a trimmed scratch save area must not leave the real run pushing
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* The scratch save area of a frame with no FP drops the slot of an instruction
 * where the phase-3 fixup vacated a register (codegen.c).  Counting a vacated
 * register the real run's picker cannot take there -- an operand it excludes,
 * one the reassigned blocker's new home displaced, or LR the dry run borrowed
 * in what turns out a leaf -- left the real run with no slot and it fell back
 * to PUSH/POP around the instruction, inside loops and next to SP-relative
 * stores (gcc torture 20041011-1's t1 too, though only in that file's own
 * layout).  Neither function may PUSH in its body. */

#include <stdio.h>

/* 471_deref_cse_nop_join's span: the vacated r0 is excluded at the compare. */
struct B { int s; int e; int pad[14]; };
struct C { struct B *blocks; int n; };

__attribute__((noinline)) int span(struct C *cfg, unsigned char *member)
{
  int lo = 1000, hi = -1;
  for (int b = 0; b < cfg->n; b++) {
    if (!member[b])
      continue;
    if (cfg->blocks[b].s < lo)
      lo = cfg->blocks[b].s;
    if (cfg->blocks[b].e - 1 > hi)
      hi = cfg->blocks[b].e - 1;
  }
  return lo * 1000 + hi;
}

/* pr53645-2's sr44444444: the blocker moved into the register the dry run
 * took as its other scratch. */
typedef short int SV __attribute__((vector_size(16)));

__attribute__((noinline)) void srem4(SV *x, SV *y)
{
  *x = *y % ((SV){4, 4, 4, 4, 4, 4, 4, 4});
}

int main(void)
{
  struct B blocks[4] = {{.s = 50, .e = 7}, {.s = 9, .e = 3}, {.s = 70, .e = 12}, {.s = 5, .e = 40}};
  struct C cfg = {blocks, 4};
  unsigned char all[4] = {1, 1, 1, 1};
  unsigned char some[4] = {1, 0, 1, 0};
  printf("span all=%d some=%d\n", span(&cfg, all), span(&cfg, some));

  SV y = {73, -9123, 32761, 8191, 16371, 1201, 12701, -9999};
  SV r;
  srem4(&r, &y);
  for (int i = 0; i < 8; i++)
    printf("%d%c", r[i], i == 7 ? '\n' : ' ');

  return 0;
}
