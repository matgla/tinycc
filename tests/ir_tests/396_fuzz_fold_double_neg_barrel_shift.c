/* Fuzz seed 39409 (-O1/-O2 wrong code; it diverged in ALL 16 generator
 * profiles, so the culprit sat in the profile-independent helper stream).
 *
 * `lr = -( (lr - (pa << 20)) | 0 )` with `lr` already known-0 lowers to
 *   T1 <- #0 SUB P0        + barrel-shift annotation  LSL #20 on src2
 *   T9 <- #0 SUB T1
 * ssa:fold's fold_double_neg matched `#0 SUB (#0 SUB z)` and rewrote T9 to
 * `ASSIGN P0`, lifting the inner SUB's src2 out WITHOUT its fused barrel
 * shift — so `pa << 20` silently became `pa` (0x56800000 -> 0x01234568) and
 * every later use of lr read the unshifted value.  ssa:const_prop_tmp was
 * only the enabler (it substituted the literal #0 into src1 that the pattern
 * needs), which is why -fno knobs pointed there.
 *
 * Fix: fold_double_neg (and the same-class fold_bitcomp_src1/src2) bail when
 * either the matched instruction or its paired def carries a barrel-shift
 * annotation, matching the guard its neighbouring folds already had.
 * Correct = gcc -m32 -funsigned-char = tcc -O0. */
#include <stdio.h>

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = 0u;
  lr = (unsigned)(-(unsigned)((lr - (unsigned)(pa << 20u)) | 0u));
  return (unsigned)(1378798602u <= (3981756465u ^ lr)) ^ lr ^ pb;
}

int main(void)
{
  printf("checksum=%08x\n", helper2(19088744u, 0xd00a4a81u));
  return 0;
}
