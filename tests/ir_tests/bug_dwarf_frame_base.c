/*
 *  TCC - Tiny C Compiler
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/* Debug info of a frame that pushes registers (test_debug_frame_gdb.py stops
 * gdb in `leaf` and reads `work`'s locals one frame up).  The frame base was
 * DW_OP_call_frame_cfa with no .debug_frame, and the fbreg offsets are from the
 * frame top (sp + locals), so every local of such a frame read wrong and gdb
 * could not unwind.  Fixed: frame base sp + frame top, and .debug_frame CFI. */

#include <stdio.h>

struct point
{
  int x;
  int y;
};

volatile int sink;

__attribute__((noinline)) int leaf(int v)
{
  sink = v;
  return v;
}

/* k's address escapes here, so k stays in work's frame at every level. */
__attribute__((noinline)) int sum3(const int *v)
{
  return v[0] + v[1] + v[2];
}

__attribute__((noinline)) int work(int a)
{
  struct point p = {a, 2 * a};
  int k[3] = {a + 1, a + 2, a + 3};
  int r = leaf(p.x + p.y + sum3(k));
  return r + a + k[2]; /* a lives across the call to leaf */
}

int main(void)
{
  printf("%d\n", work(5));
  return 0;
}
