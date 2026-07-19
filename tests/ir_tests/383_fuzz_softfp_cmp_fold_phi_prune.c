/* Fuzz seed fp_round:10 (-O1, the dominant FP-profile class): the SSA-time
 * ssa:const_prop_tmp wrapper reuses the flat core, whose
 * cpt_try_softfp_cmp_fold decides a constant __aeabi_c[df]cmp + JUMPIF and
 * rewrites it to JUMP/NOP — a CFG rewrite — but the wrapper never called
 * ssa_opt_prune_unreachable_phis ("every pass that folds a branch must call
 * this").  The clamp diamond's dead arm stayed in block_phis, ra_resolve_phis
 * emitted BOTH arm copies onto the now-straight-line path, and the dead arm's
 * copy (whose def DCE had dropped) clobbered the live value of f9. */
#include <stdio.h>
#include <string.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
int main(void)
{
  double f9 = 0x1.0c9de60000000p+40;
  f9 = (-0x1.8f43ac0000000p+29) - (-0x1.41e8940000000p+39);
  f9 = (f9 < -0x1p40 || f9 > 0x1p40) ? (double)1 : f9;
  printf("checksum=%08x\n", fbits_d(f9));
  return 0;
}
