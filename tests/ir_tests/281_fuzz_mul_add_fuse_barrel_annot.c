/*
 * ptr fuzz seed 23598 reduction (O1/O2): `(*p >> 18)` is a single-use SHR
 * that tcc_ir_barrel_shift_fusion folds into the consuming ADD's src2 as a
 * hidden `LSR #18` annotation (ir->barrel_shifts[], keyed by orig_index),
 * NOPing the SHR and leaving the deref operand raw.  The codegen MUL+ADD
 * peephole (ir/codegen.c) then saw `T = cs MUL #6` feeding that same ADD and
 * emitted the fused shifted-add sequence (`add.w r, r, r, lsl #1`), which
 * bypasses the annotated path entirely — the LSR #18 was silently dropped
 * and `*p` was added unshifted.
 * Fixed by skipping the MUL+ADD fusion when the consumer ADD carries a
 * barrel-shift annotation.
 * Ground truth (tcc -O0 == gcc -O2): checksum=3b831f52.
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
  unsigned arr[2] = { 0x4ced1234u, 9u };
  unsigned *p = &arr[0];
  cs = csmix(cs, arr[1]);
  /* (*p >> 18) is a single-use SHR the barrel-shift fusion folds into the
   * ADD's src2 (deref operand, hidden LSR #18); cs*6 is a MUL-by-const
   * feeding the same ADD, which the codegen MUL+ADD peephole fuses. */
  unsigned v = (*p >> 18) + cs * 6u;
  cs = csmix(cs, v);
  printf("checksum=%08x\n", cs);
  return 0;
}
