/* Guard: sl_forward must treat `V <-- p***DEREF*** [LOAD]` as a redefinition
 * of V.
 *
 * The pass tracks stores so a later read of the same location can be forwarded
 * the stored value.  Its "this instruction redefines a tracked vreg" scan
 * deliberately skips STORE (the store path owns those entries) and skipped
 * LOAD as well -- justified for the ordinary `T <-- addr [LOAD]` shape, whose
 * TEMP dest is nothing an entry is keyed on.  But `u = *p;` on a local that
 * lives in a VAR lowers to a LOAD whose DESTINATION is that VAR, and skipping
 * it left the earlier store entry live:
 *
 *     V1 <-- #1            [STORE]   *p11 = 1        (p11 == &u7)
 *     V1 <-- T7***DEREF*** [LOAD]    u7   = *p10     <- redefines u7
 *     V6 <-- V1            [STORE]   csmix(cs, *p11) -> forwarded back to #1
 *
 * so the read saw the pointer store's value instead of the load's.  The fix
 * lets a LOAD into a non-lval VAR/PARAM dest fall into the invalidation scan.
 *
 * Reduced from ptr fuzz seed 1 (diverged at -O2) with a two-compiler oracle, so
 * this program diverges on the unfixed build and agrees on the fixed one.
 * p12/arr8 are load-bearing: without that second aliasing pointer the store
 * entry for u7 is never created in the form the bug needs.  Expected output is
 * the -O0/gcc checksum.
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
  unsigned u7 = 2170177477u;
  unsigned arr8[8] = { 3664843848u, 2062559164u, 1736404157u, 2855051695u, 3170611280u, 371375481u, 3607564414u, 1591382744u };
  unsigned arr9[8] = { 3147205187u, 2015714664u, 2483246605u, 2779514584u, 2355095800u, 1517300986u, 3907368033u, 3132749951u };
  unsigned *p10 = &arr9[7u];
  unsigned *p11 = &u7;
  unsigned *p12 = &arr8[((unsigned)(u7) & 7u)];
  *p12 = (unsigned)1u;
  *p11 = (unsigned)1u;
  u7 = (unsigned)((*p10)) & 0xffffffffu;
  cs = csmix(cs, *p11);
  printf("checksum=%08x\n", cs);
  return 0;
}
