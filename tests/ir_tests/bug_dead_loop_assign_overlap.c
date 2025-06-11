/* Regression: dead-loop elimination (ir/opt_dce.c) placed a constant VAR
 * assignment by reusing an existing NOP instruction slot and writing dest+src1
 * via that slot's STALE operand_base.  When the NOP'd instruction had owned
 * fewer than two operand-pool slots, the src1 write overflowed into the NEXT
 * instruction's dest, corrupting it into an immediate.  At codegen this
 * crashed with "mach_get_dest_reg: unexpected kind 3" (only at -O1 and above).
 *
 * The trigger is a fixpoint loop: a bounded outer `while (changed && guard++<N)`
 * around an inner bounds-checked store loop.  Fixed by allocating fresh operand
 * slots for the new ASSIGN instead of reusing the NOP slot's old operand_base.
 */
#include <stdio.h>

static int fill(int n, signed char *vp, int total, const int *idx)
{
  int changed = 1, guard = 0, writes = 0;
  while (changed && guard++ < 64) {
    changed = 0;
    for (int i = 0; i < n; i++) {
      int dbit = idx[i];
      if (dbit < 0 || dbit >= total) continue;
      if (vp[dbit] != 7) { vp[dbit] = 7; changed++; }
      writes++;
    }
  }
  return writes;
}

int main(void)
{
  signed char vp[8] = {0};
  int idx[5] = {0, 3, -1, 9, 5};   /* two out-of-range entries are skipped */
  int writes = fill(5, vp, 8, idx);
  int sum = 0;
  for (int i = 0; i < 8; i++) sum += vp[i];
  /* slots 0,3,5 set to 7 => sum 21; one round of 3 changing writes then a
   * stable round of 3 no-op writes => writes 6. */
  printf("sum=%d writes=%d\n", sum, writes);
  return (sum == 21 && writes == 6) ? 0 : 1;
}
