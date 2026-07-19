/* Guard: ssa:cprop's copy forwarding must not fire when the copy's SOURCE has
 * more than one definition.
 *
 * cprop_assign rewrites `T3 <-- T17 [ASSIGN]` by replacing every use of T3 with
 * T17.  In real SSA that is unconditionally safe, so the rule only checked that
 * the DEST was single-def.  But a TEMP is not automatically SSA here: once a
 * local has been register-promoted, an assignment to it lowers to an in-place
 *
 *     T17 <-- #b [STORE]
 *
 * which is a second definition under the SAME name.  Forwarding then hands a
 * use sitting AFTER that store the NEW value where the copy captured the old
 * one.  This program's main() reduces to exactly that:
 *
 *     T17 <-- #-1272741064 [ASSIGN]    u5 = a
 *     T3  <-- T17          [ASSIGN]    u4 = u5      <- copies the OLD value
 *     T17 <-- #-1544475025 [STORE]     u5 = b
 *     PARAM1[call_0] T3                csmix(cs,u4) -> became T3 := T17 == b
 *
 * so csmix() was fed u5's later value instead of u4.  The fix also requires the
 * source's def_count to be 1; the two sibling copy rules in cprop.c already
 * guard this with an explicit redefinition scan.
 *
 * Reduced from ptr fuzz seed 2513, which diverged at -O1 only (-O0/-O2 agreed).
 * Kept verbatim from the reducer output: the empty if/else shell and the exact
 * constants are load-bearing.  Writing the same idea by hand with `volatile`
 * guards does NOT reproduce -- the volatile reads change the shape before the
 * SSA pipeline sees it.  Expected output is the -O0/gcc checksum.
 */

#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + 1u + (h >> 2);
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
  if ((unsigned)0u & 1u) {
    if ((unsigned)1u & 1u) {
    }
  }
  *p9 = (unsigned)(u5);
  *p7 = (unsigned)(((unsigned)((unsigned)(s3)) | (unsigned)(2750492271u)));
  cs = csmix(cs, *p9);
  printf("checksum=%08x\n", cs);
  return 0;
}
