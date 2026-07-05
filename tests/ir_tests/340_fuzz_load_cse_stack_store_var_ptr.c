/* ptr fuzz seed 380495 (O1/O2 wrong) — ssa:load_cse.
 *
 * Root cause: the canonical TEMP-DEREF LOAD CSE reuses the `iload` tracker with
 * a base that may be a VAR pointer holding the address of a *local* array
 * (here p5 = &arr4[u3&7]).  But ssa_opt_load_cse skipped all iload invalidation
 * for STACKOFF-dest stores (store_aliases_globals=0), a shortcut only sound when
 * iload entries name global/heap memory.  So a direct stack store `arr4[1]=...`
 * (which is *p5) failed to kill the cached `*p5` load, and a later `*p5` read
 * folded to the stale pre-store value.
 *
 * Fix: iload_kill_for_direct_stack_store() — a STACKOFF store invalidates iload
 * entries whose base is a VAR pointer (may point into the frame) or a TEMP that
 * resolves to an overlapping stack slot; global/heap and unchanged PARAM bases
 * are preserved.
 *
 * Correct checksum (tcc -O0 == gcc): d9b4aad7.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u2 = 3943433277u;
  unsigned u3 = 1511559249u;
  unsigned arr4[8] = { 1148523079u, 2699442515u, 1378227249u, 3549094845u, 2968291476u, 2000576624u, 1764054209u, 2150454787u };
  unsigned *p5 = &arr4[((unsigned)(u3) & 7u)];
  struct S st7 = { 2736977265u, 365248750u, 583891226u };
  struct S st8 = { 4199184628u, 1537972664u, 334476762u };
  arr4[((unsigned)(3002325554u) & 7u)] = (unsigned)((*p5));
  arr4[((unsigned)(4284578513u) & 7u)] = (unsigned)(st8.f2);
  if ((unsigned)(st8.f1) & 1u) {
  } else {
    if ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) + (unsigned)(u2))) ^ (unsigned)(((unsigned)(((unsigned)(u2) & (unsigned)(st7.f1))) >> ((unsigned)(((unsigned)(st8.f1) + (unsigned)(((unsigned)(st8.f1) ^ cs)))) & 31u))))) << ((unsigned)((*p5)) & 31u))) & 1u) {
    } else {
      u3 = (unsigned)((*p5)) & 0xffffffffu;
      *p5 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u2) | (unsigned)(((unsigned)(st8.f1) / ((unsigned)(((unsigned)(st8.f1) ^ cs)) | 1u))))) - (unsigned)((-((unsigned)(u3) | 0u))))) + (unsigned)((*p5))));
    }
  }
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr4[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
