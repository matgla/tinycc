/* Fuzz seed bitfield:310 (-O2): ssa:dead_loop's loop-invariant-phi
 * substitution built the immediate with the PHI's btype (INT8 — the latch
 * value came from a char) and installed it verbatim into every use.  A
 * STORE_INDEXED value operand sets the store width, so the word store
 * `st10.f2 = u4` was emitted as strb, leaving the field's upper 3 bytes
 * stale.  Fix: adopt the use-site btype via ssa_cprop_imm_for_use. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S { unsigned f0; unsigned f1; unsigned f2; };
int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)1;
  unsigned u4 = 695837590u;
  unsigned av = 3266478061u;
  struct S st10 = { 1576125756u, 167652608u, 1519620409u };
  for (unsigned g14 = 0u; g14 < 12u; g14++) {
    cs = csmix(cs, g14);
    u4 = (unsigned)s2;
  }
  for (unsigned g22 = 0u; g22 < 6u; g22++) {
    cs = csmix(cs, g22);
    st10.f2 = u4;
    cs = csmix(cs, g22 << ((av << 13u) & 31u));
  }
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
