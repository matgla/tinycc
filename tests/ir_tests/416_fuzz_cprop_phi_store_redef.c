/* Guard: the "is this value single-def" test in ssa:cprop must count the PHI
 * definition, not just instruction definitions.
 *
 * Companion to 414.  That test pinned cprop_assign forwarding a copy whose
 * source is re-defined in place by a `T <-- x [STORE]`; the guard added there
 * asked IRSSAVregInfo::def_count.  But def_count counts INSTRUCTION defs only --
 * a phi records itself in def_phi_block and leaves def_count alone.  So a value
 * defined by a phi and then stored to reports def_count == 1 and reads as clean
 * SSA, and the same unsound forward happens one shape further along:
 *
 *     T21 = phi(...)                      u5 after the if
 *     T9  <-- T21           [ASSIGN]      u4 = u5     <- copies the OLD value
 *     T21 <-- #-1544475025  [STORE]       u5 = ...
 *     T34 <-- T9                          -> rewritten to T21
 *
 * ssa_opt_def_total() adds the phi back in, and both the dest and the source
 * test now use it.
 *
 * Reduced from ptr fuzz seed 2513 (diverged at -O1) with a two-compiler oracle,
 * so it diverges on the unfixed build and agrees on the fixed one.  The `*p7 = 1`
 * inside the nested if is what creates the phi -- drop it and this collapses
 * back into the 414 shape.  Expected output is the -O0/gcc checksum.
 */

#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + 1u + 1u;
  return h * 2654435761u;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)1u;
  unsigned u4 = 2772724947u;
  unsigned u5 = 3022226232u;
  unsigned *p7 = &u5;
  unsigned *p9 = &u4;
  if ((unsigned)((((unsigned)((*p7)) & 1u) ? (unsigned)1u : (unsigned)1u)) & 1u) {
    if ((unsigned)1u & 1u) {
      *p7 = (unsigned)1u;
    }
  }
  *p9 = (unsigned)(u5);
  *p7 = (unsigned)(((unsigned)((unsigned)(s3)) | (unsigned)(2750492271u)));
  cs = csmix(cs, *p9);
  printf("checksum=%08x\n", cs);
  return 0;
}
