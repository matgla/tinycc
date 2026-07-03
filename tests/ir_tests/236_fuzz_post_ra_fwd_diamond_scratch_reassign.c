/* Regression: tcc_ir_opt_post_ra_forward_diamond (ir/opt_promote.c) drops a
 * phi-resolution copy on a JUMPIF's fall-through edge whenever the copy's
 * dest/src vregs happen to share a physical register at the time the pass
 * runs -- it invert-and-retargets the JUMPIF straight to the merge block and
 * NOPs the "no-op" copy instead of leaving it in place.
 *
 * That "same register" snapshot is taken from ir->ls.intervals[] /
 * tcc_ir_vreg_live_interval() *before* codegen's Phase-3 scratch-conflict
 * fixup (try_reassign_scratch_conflict, ir/codegen.c) runs.  That fixup can
 * independently move just the copy's dest interval to a different physical
 * register later (to avoid a spill at some other instruction), without ever
 * seeing that the copy which used to keep the two vregs in sync no longer
 * exists.  The fall-through edge then reads a register that was never
 * written on that path -- the classic dropped-phi-copy shape (an `if` with
 * no `else` reads garbage instead of the pre-`if` value).
 *
 * Fix: post_ra_forward_diamond now marks both the dest and src intervals of
 * every no-op copy it eliminates as phi_pinned, the same guard
 * ra_phi_copy_needed() already sets for its own post-RA identity-copy case,
 * so try_reassign_scratch_conflict refuses to move either one afterward.
 *
 * Reduced from tests/fuzz/fuzz_triage_repros/switch_seed822.c (profile=switch,
 * seed 822).  Diverges at -O2 only; tcc -O0/-O1 and
 * gcc -m32 -funsigned-char both agree on checksum=19dab0f8.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s1 = (char)(702726770u & 0xff);
  long s2 = (long)(1272027647u & 0xffffffff);
  unsigned u3 = 2849304672u;
  unsigned u4 = 2680506947u;
  unsigned u5 = 94983404u;
  unsigned u6 = 4270078760u;
  unsigned u7 = 2163344085u;
  unsigned arr8[8] = { 585553713u, 3996972145u, 309607912u, 2630940704u, 4084828711u, 3621702511u, 4029305485u, 848605956u };
  if ((unsigned)((-((unsigned)(((unsigned)((-((unsigned)(1312518524u) | 0u))) <= ((unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) + (unsigned)(((unsigned)(1002566331u) > ((unsigned)(u6) ^ cs))))) ^ cs))) | 0u))) & 1u) {
    if ((unsigned)(u4) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s1)) / ((unsigned)(((unsigned)((unsigned)(s1)) ^ cs)) | 1u))));
    }
  } else {
    u3 = (unsigned)((((unsigned)((((unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(((unsigned)((unsigned)(s1)) << ((unsigned)(((unsigned)((unsigned)(s1)) ^ cs)) & 31u))) : (unsigned)((-((unsigned)(u6) | 0u))))) & 1u) ? (unsigned)((-((unsigned)((unsigned)(s2)) | 0u))) : (unsigned)(u4))) & 1u) ? (unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) % ((unsigned)(((unsigned)(((unsigned)(3293087189u) - (unsigned)(2178902828u))) & (unsigned)(((unsigned)(u3) | (unsigned)(arr8[((unsigned)(u7) & 7u)]))))) | 1u))) : (unsigned)(((unsigned)(((unsigned)((~((unsigned)(1349024527u) | 0u))) + (unsigned)(((unsigned)(3510828889u) ^ (unsigned)(u6))))) + (unsigned)(((unsigned)(((unsigned)(1677132774u) - (unsigned)(u7))) >> ((unsigned)(((unsigned)(562462976u) >= ((unsigned)(u6) ^ cs))) & 31u))))))) & 0xffffffffu;
    { unsigned sel9 = (unsigned)(((unsigned)(u7) == ((unsigned)((-((unsigned)((unsigned)(s1)) | 0u))) ^ cs))) & 63u;
      switch (sel9) {
      case 4:
        arr8[((unsigned)(u3) & 7u)] = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(2801971572u) & 7u)]) + (unsigned)(u6))) | (unsigned)(arr8[((unsigned)(u5) & 7u)]))) ^ (unsigned)(((unsigned)(arr8[((unsigned)(3804300176u) & 7u)]) ^ (unsigned)(((unsigned)(u7) - (unsigned)(871583839u))))))) >> ((unsigned)(arr8[((unsigned)(1076729254u) & 7u)]) & 31u)));
        cs = csmix(cs, 2197022917u);
        cs = csmix(cs, 2251375517u);
        cs = csmix(cs, (unsigned)((~((unsigned)((-((unsigned)(arr8[((unsigned)(u3) & 7u)]) | 0u))) | 0u))));
        cs = csmix(cs, 3964360506u);
        cs = csmix(cs, (unsigned)(((unsigned)(817821172u) << ((unsigned)(arr8[((unsigned)(u4) & 7u)]) & 31u))));
        cs = csmix(cs, 3656499978u);
        cs = csmix(cs, 1978231881u);
        cs = csmix(cs, 4100498166u);
        cs = csmix(cs, 3895082624u);
      default: cs = csmix(cs, 104u); break;
      } }
    { unsigned g10 = (unsigned)(((unsigned)(3132429573u) & (unsigned)((~((unsigned)((unsigned)(s1)) | 0u))))) & 1u;
      cs = csmix(cs, (unsigned)(((unsigned)(2429475531u) << ((unsigned)(((unsigned)(((unsigned)((-((unsigned)(arr8[((unsigned)(u7) & 7u)]) | 0u))) * (unsigned)((((unsigned)(383494213u) & 1u) ? (unsigned)(415646730u) : (unsigned)(4222035744u))))) ^ (unsigned)(((unsigned)(u7) + (unsigned)(((unsigned)(u6) >> ((unsigned)(((unsigned)(u6) ^ cs)) & 31u))))))) & 31u))));
      cs = csmix(cs, (unsigned)(((unsigned)(u7) ^ (unsigned)(((unsigned)((unsigned)(s1)) / ((unsigned)(((unsigned)(((unsigned)(2017260760u) % ((unsigned)(u3) | 1u))) >= ((unsigned)(975877534u) ^ cs))) | 1u))))));
      cs = csmix(cs, 99u); }
    if ((unsigned)(((unsigned)(((unsigned)(4255012269u) >= ((unsigned)((-((unsigned)(((unsigned)(u5) & (unsigned)(arr8[((unsigned)(2475930700u) & 7u)]))) | 0u))) ^ cs))) / ((unsigned)(arr8[((unsigned)(u5) & 7u)]) | 1u))) & 1u) {
      u7 = (unsigned)(arr8[((unsigned)(u4) & 7u)]) & 0xffffffffu;
      u3 = (unsigned)(((unsigned)(u6) != ((unsigned)(u5) ^ cs))) & 0xffffffffu;
    }
    arr8[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)((~((unsigned)(((unsigned)((unsigned)(s1)) + (unsigned)(u4))) | 0u))) / ((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(((unsigned)(1636324746u) % ((unsigned)(u7) | 1u))) & 31u))) | 1u)));
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
