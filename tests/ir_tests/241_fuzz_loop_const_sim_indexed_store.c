/* Regression: loop_const_sim seeded a stack slot's pre-loop value from an
 * earlier DIRECT store and never saw a later STORE_INDEXED overwrite it, so a
 * loop that copied that slot folded to the stale initializer constant.
 *
 * From agg_deep fuzz seed 47 (reduced).  tcc -O0/-O1/-Os agreed with gcc; only
 * tcc -O2 diverged — an O2 optimizer miscompile in the loop-constant simulator.
 *
 * Root cause: `st12.f2 = st12.f0 ^ *p` lowers to a `STORE_INDEXED #4` off the
 * base address `&st12` (i.e. it writes st12.f2's slot at &st12+4).  The pre-loop
 * seeding in lcs_init_var_state (ir/opt_loop_const_sim.c) only models direct
 * `StackLoc[off] <- imm` stores and indirect stores through a known-address
 * temp — it did not handle STORE_INDEXED.  It therefore kept st12.f2's slot at
 * its INITIALIZER constant (1548461477) and, when the loop body `st12.f0 =
 * st12.f2` was constant-folded, produced `st12.f0 = 1548461477` instead of the
 * recomputed `st12.f0 ^ *p`.  The pointer deref forces the STORE_INDEXED form;
 * a plain scalar (or `^ u8` without the pointer) stays a direct store and was
 * modeled correctly, which is why the bug needs the struct member + `*p`.
 *
 * Fix: a pre-loop STORE_INDEXED / STORE_POSTINC / BLOCK_COPY now conservatively
 * demotes every tracked memory slot to flow-unsafe (its target offset cannot be
 * resolved), so the simulator never trusts a stale initial value.
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
  unsigned u8 = 1775873863u;
  struct S st12 = { 4007732392u, 3195348664u, 1548461477u };
  unsigned *p = &u8;

  /* STORE_INDEXED #4 off &st12: writes st12.f2's slot with a recomputed value. */
  st12.f2 = st12.f0 ^ (*p);
  /* Loop with an invariant store of st12.f2 into st12.f0; the constant simulator
   * must use the RECOMPUTED st12.f2, not its stale initializer 1548461477. */
  for (unsigned g = 0u; g < 12u; g++) {
    cs = csmix(cs, g);
    st12.f0 = st12.f2;
  }
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
