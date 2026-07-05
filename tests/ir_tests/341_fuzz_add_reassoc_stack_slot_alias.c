/* ptr fuzz seed 409667 (O1/O2 wrong) — add_reassoc.
 *
 * Root cause: add_reassoc folds `x = u4 + C2` into `x = base + (C1+C2)` when the
 * defining instruction is `u4 = base + C1`.  Its aliasing guard bails only when
 * `base` (or the use's own vreg) is an address-taken VAR, but here `base` is a
 * *direct stack slot* — `StackLoc[-4]` == arr6[7], a raw STACKOFF operand with
 * no backing vreg (inner_vr < 0).  An intervening `*p7 = ...` store (p7 ==
 * &arr6[7]) overwrites that slot in the gap, so re-reading `arr6[7] + combined`
 * at the later use picks up the post-store value instead of the value u4 was
 * computed from — a stale-load miscompile.
 *
 * Fix (ir/opt_constprop.c tcc_ir_opt_add_reassoc): also bail when a
 * memory-clobbering op sits in the gap and def_src1 is a direct memory-slot load
 * (is_lval with inner_vr < 0), extending the seed-85636 aliasing guard to raw
 * StackLoc bases that carry no vreg.
 *
 * Correct checksum (tcc -O0 == -O1 == -O2 == -Os): 50ccf4e3.
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
  char s3 = (char)(435301073u & 0xff);
  unsigned u4 = 1379253785u;
  unsigned u5 = 3738482691u;
  unsigned arr6[8] = { 1937651021u, 2017833479u, 599590024u, 3271876917u, 695199786u, 3966865249u, 2415746788u, 2537698353u };
  unsigned *p7 = &arr6[7u];
  struct S st8 = { 1160484591u, 398500312u, 3930666701u };

  for (unsigned g10 = 0u; g10 < 12u; g10++) {
    unsigned i9 = g10;
    u4 = (unsigned)(((unsigned)(arr6[((unsigned)(2269000735u) & 7u)]) + (unsigned)((unsigned)(s3)))) & 0xffffffffu;
    *p7 = (unsigned)((~((unsigned)(((unsigned)(u4) << ((unsigned)(1451368024u) & 31u))) | 0u)));
    *p7 = (unsigned)(((unsigned)(((unsigned)(st8.f0) - (unsigned)(u5))) * (unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) <= ((unsigned)(3925937864u) ^ cs))) - (unsigned)(((unsigned)(u4) - (unsigned)(3620195667u))))) | (unsigned)((((unsigned)(st8.f0) & 1u) ? (unsigned)(i9) : (unsigned)(((unsigned)(3494815608u) ^ (unsigned)((unsigned)(s3))))))))));
  }

  cs = csmix(cs, *p7);
  printf("checksum=%08x\n", cs);
  return 0;
}
