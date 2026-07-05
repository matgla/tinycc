/* Regression: loop_const_sim's pre-loop scan missed an indirect store whose
 * address was `Addr[StackLoc] + immediate`, so it kept a stack array element's
 * stale .data initializer and simulated the loop with the wrong value.
 *
 * From combo_num fuzz seed 872 (reduced).  tcc -O0/-O1 agreed with gcc; tcc -O2
 * diverged — an O2 loop-const-simulation miscompile (bisect_opt named const-prop
 * + loop-unroll as the fixing knobs; the constant fold first appeared at the
 * loop_const_sim pass).
 *
 * Root cause: `arr12[u11 & 7] = <runtime>` with u11 a constant-valued *variable*
 * lowers to `T = Addr[&arr12] ADD #4 ; *T = <runtime>` (constant offset, but an
 * indirect store through a computed pointer).  loop_const_sim's pre-loop scan
 * (lcs_init_var_state in ir/opt_loop_const_sim.c) recognised stack addresses
 * only from ASSIGN/LOAD/LEA, not from ADD/SUB, so `T` was demoted to unknown and
 * the indirect store could not be resolved to arr12[1].  arr12[1] therefore kept
 * its initializer 3476322611 in the memory map, and when the trailing
 * `for (k<2) cs = csmix(cs, arr12[k])` loop was unrolled/simulated it folded the
 * checksum using that stale value instead of the just-stored one.
 *
 * Fix: the pre-loop scan now models `T = <stack address> +/- immediate` as a
 * stack address (mirroring lcs_step's ADD/SUB address arithmetic), so the
 * indirect store is resolved and arr12[1] is correctly demoted.
 *
 * u11 & 7 == 1 == u9 & 7, so the store and the k==1 read both touch arr12[1].
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb){ (void)pa; return 1337140448u ^ (unsigned)(pb); }
static unsigned helper3(unsigned pa, unsigned pb){ unsigned lr = pa ^ (pb * 3u); return 3868881785u ^ lr; }
int main(void)
{
  unsigned cs = 0x12345678u;
  short s5 = (short)(1906163233u & 0xffff);
  unsigned u7 = 1079883352u;
  unsigned u9 = 2166282241u;
  unsigned u11 = 986589497u;
  unsigned arr12[8] = { 407849359u, 3476322611u, 2976537358u, 3237948049u, 1905294153u, 2833875773u, 4212026209u, 1441732214u };
  /* arr12[u11&7] (== arr12[1]) written with a RUNTIME value; lowers to
     T = Addr[&arr12] ADD #4 ; *T = <runtime>. */
  arr12[u11 & 7u] = arr12[u9 & 7u] % ((((~2585896424u) % ((unsigned)(s5) | 1u)) << ((helper1(u11, u7) / (helper3(1779398331u, u7) | 1u)) & 31u)) | 1u);
  /* unrolled at O2 -> loop_const_sim simulates reading arr12[1] */
  for (unsigned k = 0u; k < 2u; k++) cs = csmix(cs, arr12[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
