/* Regression: SWITCH_TABLE (jump-table) dispatch clobbers R_IP (R12), but the
 * linear-scan allocator kept a loop-carried value live across the dispatch in
 * R12 -> the dispatch's `LSL/ADD/LDR/ADD/BX ip` preamble corrupted it.
 *
 * Profile=switch, fuzz seed 102 (reduced).  At -O2 the rolling checksum `cs` is
 * loop-carried and `csmix` is inlined (high register pressure), so the
 * allocator placed `cs` in R12 -- exactly the scratch the jump-table dispatch
 * (tcc_gen_machine_switch_table_mop in arm-thumb-gen.c) overwrites with the
 * table base.  Every case body then read the clobbered `cs`, so the checksum
 * diverged at -O2 only (-O0/-O1 keep `cs` in a callee-saved register because
 * csmix is not inlined there).
 *
 * Fix: ir/regalloc.c marks any interval live across a SWITCH_TABLE/SWITCH_LOAD
 * as crosses_call, forcing it off the caller-saved R12 (into a callee-saved
 * register or spill) -- exactly what -O1 already does.
 *
 * Ground truth gcc -m32 -funsigned-char == tcc -O0 == checksum=f945410e.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

struct S { unsigned f0; unsigned f1; unsigned f2; };

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u4 = 1846930904u;
  unsigned u5 = 2946289266u;
  struct S st7 = { 3407772209u, 3178690981u, 1112538255u };

  unsigned g9 = 0u;
  while (g9 < 11u) {
    st7.f0 = u5;
    unsigned sel10 = (u5 + g9) & 7u;       /* >=4 dense cases -> O1+ jump table */
    switch (sel10) {
    case 0: cs = csmix(cs, u5); break;
    case 1: cs = csmix(cs, 1456509558u); break;
    case 2: st7.f2 = st7.f0; cs = csmix(cs, 1942302789u); break;
    case 3: cs = csmix(cs, 4249354386u); break;
    case 4: u4 = 991044994u; cs = csmix(cs, u5); break;
    default: cs = csmix(cs, 163u); break;
    }
    g9++;
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
