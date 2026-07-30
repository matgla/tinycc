/* Guard: sl_forward must invalidate a store entry when a CALL returns directly
 * into the variable that entry is keyed on.
 *
 * Companion to 415.  The pass keys each tracked store on the variable written
 * (store_dest_vr) and separately remembers which vreg supplied the value
 * (stored_value).  Its FUNCCALLVAL handler only ever compared the call's dest
 * against stored_value -- "did the call clobber the value I would forward?" --
 * and never against store_dest_vr, "did the call overwrite the variable
 * itself?".  The general redefinition scan further down checks both; the call
 * path returns early, so it never got there:
 *
 *     V2  <-- #1   [STORE]           *p11 = 1        (p11 == &u8)
 *     CALL helper2 --> V2            u8   = helper2(...)   <- overwrites u8
 *     V10 <-- V2   [STORE]           csmix(cs, *p11) -> forwarded back to #1
 *
 * so the read saw 1 instead of the call's result.  The fix mirrors the general
 * scan: a call whose dest is a non-lval VAR/PARAM kills entries keyed on it.
 *
 * Reduced from ptr fuzz seed 1410 (diverged at -O2).  Expected output is the
 * -O0/gcc checksum.
 */

#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + 1u + 1u;
  return h * 2654435761u;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)1u != ((unsigned)(((unsigned)(((unsigned)(pa) & (unsigned)(((unsigned)(pa) ^ lr)))) | (unsigned)(((unsigned)(pb) * (unsigned)(206113354u))))) ^ lr))) ^ lr;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u5 = 807849282u;
  unsigned u8 = 2392413023u;
  unsigned *p10 = &u5;
  unsigned *p11 = &u8;
  for (unsigned g16 = 0u; g16 < 2u; g16++) {
    { unsigned g18 = 0u;
      while (g18 < 9u) {
        g18++;
      }
    }
  }
  if ((unsigned)0u & 1u) {
    { unsigned g20 = 0u;
      while (g20 < 7u) {
      }
    }
    { unsigned g22 = 0u;
      while (g22 < 1u) {
      }
    }
  }
  *p11 = (unsigned)1u;
  u8 = (unsigned)(helper2(2069332987u, ((unsigned)1u - (unsigned)((*p10))))) & 0xffffffffu;
  cs = csmix(cs, *p11);
  printf("checksum=%08x\n", cs);
  return 0;
}

