/* Fuzz seed agg_deep:43933 (-O2 wrong code).
 *
 * `m[i15][2]` lowers to `LOAD_INDEXED(T6, #8)` with T6 = &m[0][0] + (i15 << 4)
 * — a frame address plus a RUNTIME term, so ssa_opt_resolve_lea_stackloc()
 * cannot pin it to an exact stack offset.  ssa:load_cse's
 * iload_kill_for_direct_stack_store / iload_kill_for_stack_store read that
 * failure as "this TEMP base names global or heap memory, a local store can't
 * reach it" and kept the entry live across `m[0][2] <- …` (a direct
 * `StackLoc[-56] <- T9` store).  The second `m[i15][2]` was then CSE'd to the
 * pre-store load, so the i15==0 iteration read the stale value.
 *
 * Fix: unresolved is not non-aliasing.  iload_base_may_be_frame() walks the
 * address computation and only clears the kill when every term is provably
 * outside the frame (global/static symbol address, PARAM pointer, constant).
 * Correct = gcc -m32 -funsigned-char = tcc -O0. */
#include <stdio.h>

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned m[4][4] = {{0}};
  for (unsigned i13 = 0u; i13 < 4u; i13++) {
    for (unsigned i15 = 0u; i15 < 2u; i15++) {
      m[0][2] = (i13 ^ m[i15][2]) | 8u;
      cs += m[i15][2];
    }
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
