/*
 * signed fuzz seed 50156 reduction (O1/O2 wrong-code):
 * the CMP-vs-constant-offset fold `tcc_ir_opt_cmp_const_offset_fold`
 * (ir/opt_constprop.c) folds `CMP a,b` when it can prove `a = b +/- K` from
 * a's defining instruction, evaluating the signed comparison at compile time.
 * It located a's def with `tcc_ir_find_defining_instruction`, a purely LINEAR
 * backward scan that returns the nearest preceding def and is blind to a
 * back-edge redefinition of a multi-def vreg.
 *
 * Here si7 is defined THREE times: `si7 = si6 - 9033` (outer-loop body, the
 * offset def the pass latched onto), and `si7 = 659161088` at the inner-loop
 * tail, which reaches the `si7 <= si6` compare again via the inner back-edge.
 * The pass proved `si7 - si6 == -9033` from the outer def and folded
 * `si7 <= si6` to a constant true (result 1) for EVERY iteration -- but on the
 * 2nd inner iteration si7 is 659161088 (> si6), so the compare is false.
 *
 * Fix: require both CMP operands to have a single definition
 * (tcc_ir_vreg_has_single_def) before trusting the linear offset relationship,
 * mirroring the back-edge guard already used in ir_opt_eval_const_u64.
 *
 * The post-loop `si6 = (signed char)si6` is load-bearing: it keeps si6 a
 * genuine multi-def variable rather than a folded literal; with si6 constant
 * the compare is rewritten a different way and the divergence disappears.
 *
 * Ground truth (tcc -O0 == arm-none-eabi-gcc -O2): 0117b570.
 */
#include <stdio.h>

int main(void)
{
  unsigned cs = 0;
  int si6 = 3851;
  int si7 = -27456;
  for (unsigned g10 = 0u; g10 < 3u; g10++) {
    si7 = si6 - 9033;
    for (unsigned g12 = 0u; g12 < 2u; g12++) {
      cs = cs * 33u + (unsigned)((si7 <= si6) ? 1 : 0);
      si7 = 659161088;   /* 20116 << 15 */
    }
  }
  si6 = (int)(signed char)si6;
  cs += (unsigned)si6 * 7u;
  cs += (unsigned)si7 * 13u;
  printf("checksum=%08x\n", cs);
  return 0;
}
