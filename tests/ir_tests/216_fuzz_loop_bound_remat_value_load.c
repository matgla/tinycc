/* Regression: loop_bound_remat rematerialized a VALUE-LOAD of a local var.
 *
 * NOTE: loop_bound_remat was RETIRED 2026-07-07 (proven inert at its tccgen
 * site; see docs/plan_legacy_loop_bound_remat_ssa.md).  This pin is retained as
 * an anti-reintroduction guard — the checksum must stay correct at every -O
 * level should a future pass re-attempt this rematerialization.
 *
 * Pass:  tcc_ir_opt_loop_bound_remat (ir/opt_loop.c, removed), gated by -fiv-strength-red.
 * Bug:   the pass recomputes SP-relative end-POINTERS (Addr[StackLoc], is_lval=0)
 *        just before a loop CMP to shrink their live range.  It also (wrongly)
 *        accepted a candidate whose STACKOFF source was a VALUE LOAD of a named
 *        local variable (is_lval=1, is_local=1, with a live VAR vreg) — here the
 *        pre-loop read of `u8` for the `u8 <= (~cs)` test.  It rematerialized that
 *        as a fresh anonymous `StackLoc[0]` value-load (dropping the VAR identity
 *        and is_local), then NOP'd the original read.  Because `u8` is a value
 *        (register) variable with no physical home at offset 0, the rematerialized
 *        load reads uninitialized stack → `u8`'s `<=` result is wrong, corrupting
 *        the whole checksum at -O1/-O2.  -O0 (and -fno-iv-strength-red) are correct.
 * Fix:   only rematerialize address-of-stack candidates (is_lval==0).  A value
 *        load is a memory read, never an SP-relative "end pointer", and is unsound
 *        to rematerialize.
 *
 * Reduced from differential-fuzz seed 6214.  Ground truth = gcc -m32 -funsigned-char.
 * Unfixed -O1/-O2 print checksum=24417058; correct value is below.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(4112162874u) >> ((unsigned)(4127531557u) & 31u))) & (unsigned)(((unsigned)(pa) >> ((unsigned)(((unsigned)(pa) ^ lr)) & 31u))))) << ((unsigned)(((unsigned)(((unsigned)(742585238u) / ((unsigned)(3519748770u) | 1u))) - (unsigned)(((unsigned)(pb) ^ (unsigned)(2413161436u))))) & 31u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((-((unsigned)(((unsigned)(((unsigned)(2343635568u) >> ((unsigned)(2092054031u) & 31u))) - (unsigned)((~((unsigned)(pb) | 0u))))) | 0u))) ^ lr;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s3 = (int)(536707904u & 0xffffffff);
  char s4 = (char)(1587529673u & 0xff);
  unsigned u5 = 1887548698u;
  unsigned u6 = 1436962320u;
  unsigned u7 = 3478802516u;
  unsigned u8 = 3244552849u;
  cs = csmix(cs, (unsigned)(((unsigned)(2958541139u) + (unsigned)(((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u6) + (unsigned)(u5))))) * (unsigned)(u6))))));
  u8 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) >> ((unsigned)(((unsigned)(u6) & (unsigned)(1860684166u))) & 31u))) <= ((unsigned)((~((unsigned)(((unsigned)(u5) / ((unsigned)(2127999881u) | 1u))) | 0u))) ^ cs))) << ((unsigned)((unsigned)(s3)) & 31u))) & 0xffffffffu;
  { unsigned g10 = 0u;
    while (g10 < 9u) {
      unsigned i9 = g10;
      cs = csmix(cs, i9);
      for (unsigned g12 = 0u; g12 < 12u; g12++) {
        unsigned i11 = g12;
        cs = csmix(cs, i11);
        cs = csmix(cs, (unsigned)(((unsigned)(helper1(((unsigned)(819936218u) * (unsigned)(helper2(u6, u6))), ((unsigned)(2921242289u) ^ (unsigned)(((unsigned)(i9) % ((unsigned)(739859885u) | 1u)))))) * (unsigned)(((unsigned)(helper2((((unsigned)(i11) & 1u) ? (unsigned)(3985499516u) : (unsigned)(u5)), ((unsigned)((unsigned)(s3)) % ((unsigned)(u7) | 1u)))) ^ (unsigned)(((unsigned)(1049729826u) / ((unsigned)(((unsigned)(u6) >> ((unsigned)(1721372465u) & 31u))) | 1u))))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(u8) * (unsigned)(((unsigned)(i11) - (unsigned)(u5))))) >> ((unsigned)(i11) & 31u))) * (unsigned)((~((unsigned)((((unsigned)(1032449888u) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(((unsigned)(i11) | (unsigned)(1560929108u))))) | 0u))))));
        cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s3)) + (unsigned)(((unsigned)((unsigned)(s3)) ^ cs)))));
      }
      g10++;
    }
  }
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  printf("checksum=%08x\n", cs);
  return 0;
}
