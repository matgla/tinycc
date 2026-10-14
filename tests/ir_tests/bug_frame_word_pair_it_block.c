/* codegen:frame_word_pair paired the last access of an IT block with the next one.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* `const int t = k ? a : b, o = k ? b : a;` on spilled parameters lowers to
 * `ite eq; ldreq r4,[sp,#40]; ldrne r4,[sp,#36]`, and the next frame word
 * read was `ldr r1,[sp,#32]`.  frame_word_pair_rewind checks only that no IT
 * block is pending when the second access comes -- after the IT's last
 * instruction none is -- and rewound over the conditional `ldrne` to emit
 * `ldrdne r1,r4,[sp,#32]`: r1 was loaded on the `ne` path only.  The native
 * tcc's frame_objs_apart (ir/frame.c) has this shape; the device compiler
 * crashed compiling source/unity/arm.c.  Same as fix-base-2026-10-12. */
#include <stdio.h>

struct ev { int i, kind; };
struct live { const struct ev *ev; const int *first, *count, *idx; };
struct fl { const int *ops; };

__attribute__((noinline)) int live_at(struct fl *f, struct live *l, int o, int i)
{
  return (f->ops[i] + o + l->count[o]) % 7 == 0;
}

__attribute__((noinline)) int apart(struct fl *f, struct live *l, int a, int b, int lo, int hi)
{
  const int *ops = f->ops;
  for (int k = 0; k < 2; k++)
  {
    const int t = k ? a : b, o = k ? b : a;
    const int *v = l->idx + l->first[t];
    for (int j = 0; j < l->count[t]; j++)
    {
      const struct ev *e = &l->ev[v[j]];
      if (e->i >= lo && e->i <= hi)
        continue;
      if (e->kind == 1 || (e->kind == 2 && ops[e->i] == 5))
        continue;
      if (live_at(f, l, o, e->i))
        return 0;
      const int *w = l->idx + l->first[o];
      for (int m = 0; m < l->count[o]; m++)
        if (l->ev[w[m]].i == e->i)
          return 0;
    }
  }
  return 1;
}

int main(void)
{
  static const struct ev ev[8] = {{0, 0}, {1, 0}, {2, 1}, {3, 0}, {4, 2}, {5, 0}, {6, 0}, {7, 0}};
  static const int first[3] = {0, 3, 6}, count[3] = {3, 3, 2}, idx[8] = {0, 1, 3, 4, 5, 6, 2, 7};
  static const int ops[8] = {1, 2, 3, 4, 5, 6, 2, 9};
  struct live l = {ev, first, count, idx};
  struct fl f = {ops};
  int r = 0;
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      for (int lo = 0; lo < 3; lo++)
        r = r * 3 + apart(&f, &l, a, b, lo, lo + 2) + 7 * (a + b);
  printf("%d\n", r);
  return 0;
}
