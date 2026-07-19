/* Fuzz seed volatile:82433 (-O1/-O2 wrong code).
 *
 * `u4 + u4 * f2` fuses into `MLA src1=u4, src2=f2, accum=u4`, where u4 is a
 * diamond phi (the guarded `u4 = u5` below).  SCCP needs a second round to
 * prove that phi constant: the guard reads a ternary (another phi) whose value
 * comes from a SETIF that SCCP cannot evaluate, so round 1 leaves the guard
 * live -- and the MLA fusion runs at the end of round 1.
 *
 * In round 2 sccp_materialize_const_phis() proved the phi CONST and deleted
 * it, but its "every use reads dest as src1/src2" admission test only looked
 * at src1/src2 -- and this MLA matches (accum aliases src1).  The rewrite
 * (sccp_set_instr_operands_imm) likewise only rewrites src1/src2, so the
 * accumulator at operand_base+3 kept naming the now-defless temp:
 *   T77 <-- #545302529 MLA #820016041 + T92      <- T92 has no def
 * and the MLA accumulated an undefined register (observed: +2).
 *
 * Fix: bail out of the phi materialization when a use reads the phi dest in
 * MLA's accumulator slot.  Rewriting it is not an option -- ARM's
 * MLA Rd,Rn,Rm,Ra takes no immediate in Ra.
 * Correct = gcc -m32 -funsigned-char = tcc -O0. */
#include <stdio.h>

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u4 = 545302529u;
  unsigned u5 = 647248505u;
  unsigned f0 = 370617737u, f2 = 820016041u;
  unsigned vv8;

  if ((((u5 & 1u) ? f0 + (unsigned)(4273684596u >= ((f2 >> (u4 & 31u)) ^ cs)) : f2) & 1u) != 0u)
    u4 = u5;
  u5 = 3209649329u;
  vv8 = 3285198014u % ((u4 + u4 * f2) | 1u);

  printf("checksum=%08x\n", vv8);
  return 0;
}
