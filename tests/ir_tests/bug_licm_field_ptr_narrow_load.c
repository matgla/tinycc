/* licm_field_ptr hoisted a byte-wide field read as a word.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* A Form A candidate (`T <- B LOAD_INDEXED #9`) records the base operand B as
 * the operand to re-load, and the preheader load took B's INT32 type for its
 * value: `d < g_st->depth` with an unsigned char `depth` at offset 9 became
 * `ldr.w r2,[r1,#9]`, which read the three bytes after it as well.  The
 * native tcc's inline_inside_wrapper (`depth < tcc_state->inline_expansion_depth`)
 * then scanned past the live expansion stack and changed inlining decisions
 * on the device only (self-host stage 2 objects differed from the host's). */
#include <stdio.h>

struct sym { int pad[2]; struct sym *ref; int flags; };
struct st { char pad[9]; unsigned char depth; unsigned char junk[2]; struct sym *syms[8]; };
struct st *g_st;

__attribute__((noinline)) int any_wrapper(void)
{
  for (int d = 0; d < g_st->depth && d < 8; d++)
  {
    struct sym *p = g_st->syms[d];
    if (p && p->ref && (p->ref->flags & 0x40000000))
      return 1;
  }
  return 0;
}

int main(void)
{
  static struct sym plain = {{0, 0}, 0, 0}, wr = {{0, 0}, 0, 0x40000000}, a = {{0, 0}, &plain, 0}, b = {{0, 0}, &wr, 0};
  static struct st s = {{0}, 2, {0xff, 0xff}, {&a, &a, &b, &b}};
  g_st = &s;
  int x = any_wrapper();
  s.depth = 3;
  int y = any_wrapper();
  printf("%d %d\n", x, y);
  return 0;
}
