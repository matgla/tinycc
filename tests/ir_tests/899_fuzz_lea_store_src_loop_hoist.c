/*
 *  TCC Tests - Fuzz seed 2395: store-source LEA fusion hoisted a load
 *              across a loop back-edge
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* AUTO-DERIVED from tests/fuzz seed 2395 (docs/bugs/fuzz_seed_2395_o2_divergence.md).
 *
 * ssa_gen_arm_fuse_store_src_through_add_imm folds `V <- *(base + #imm)`
 * by rewriting the address ADD itself into LOAD_INDEXED, relocating the load
 * from the STORE's site up to the ADD's definition site.  LICM (-O2) had
 * hoisted the invariant address ADD out of the loop while the deref STORE and
 * the aliasing store to the same element stayed inside it; the guard scanned
 * the instruction range for stores/jumps but a fallthrough block boundary
 * carries no jump, so the loop back-edge wrapping around the in-loop store was
 * invisible.  Iteration 1 then read the pre-loop initializer instead of the
 * value stored by iteration 0.  Fix: also bail on any jump-target (join)
 * instruction between the ADD and the STORE, as ir_xform_same_block does.
 *
 * Correct output (gcc -m32 -funsigned-char oracle): checksum=113876b8
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u4 = 1164363335u;   /* u4 & 7 == 7 */
  unsigned arr7[8] = { 71172552u, 4035464020u, 878362783u, 1302692097u,
                       2348434059u, 3487570888u, 794893348u, 3715463114u };
  for (unsigned g19 = 0u; g19 < 2u; g19++) {
    unsigned i18 = g19;
    cs = csmix(cs, i18);
    cs = csmix(cs, arr7[(u4 & 7u)]);
    arr7[(u4 & 7u)] = 2445291734u * i18 + g19;
  }
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr7[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
