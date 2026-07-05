/* Regression: GVN (global value numbering) coalesced congruent 64-bit
 * computations into an ASSIGN copy, and the register-pair copy dropped the high
 * word — so a later `>> 32` read 0.
 *
 * From longlong fuzz seed 686 (reduced).  tcc -O0/-O1 agreed with gcc; tcc -O2
 * diverged (culprit knobs loop-rotation + loop-unroll; the underlying defect is
 * in ssa:gvn, which unrolling feeds).
 *
 * Root cause: unrolling the `for (g19<5) q11 = const | q12;` loop produces five
 * congruent 64-bit `q11 = q12 | const` computations.  ssa_opt_gvn keys on
 * (op, src1, src2) and rewrote four of them into ASSIGN copies of the first
 * result temp T86.  That copy of a 64-bit value (a register pair) is mishandled
 * downstream: only the low word survives, so `(unsigned)(q11 >> 32)` — which
 * extracts the high word — evaluated to 0 and the checksum used q11's low word
 * instead of low^high.
 *
 * Fix: ssa_opt_gvn no longer value-numbers 64-bit-result instructions; declining
 * this rare CSE avoids emitting a truncating pair copy (ir/opt/ssa_opt_gvn.c).
 *
 * Needs: a 64-bit value built by OR (q11 = const | q12) inside an unrolled loop,
 * read via `(unsigned)q11 ^ (unsigned)(q11 >> 32)`, plus a second short loop for
 * the register pressure that makes GVN emit the copy.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u8 = 1598063427u;
  unsigned u9 = 1122989588u;
  unsigned long long q11 = 0;
  unsigned long long q12 = (((unsigned long long)(u8)) << 32) | (unsigned long long)(u9);
  for (unsigned g17 = 0u; g17 < 5u; g17++) {
    cs = csmix(cs, g17);
    u9 = g17;
  }
  for (unsigned g19 = 0u; g19 < 5u; g19++) {
    cs = csmix(cs, g19);
    q11 = (10275325003342162569ull) | (q12);
  }
  cs = csmix(cs, (unsigned)(q11) ^ (unsigned)(q11 >> 32));
  printf("checksum=%08x\n", cs);
  return 0;
}
