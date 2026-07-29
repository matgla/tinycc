/* Guard: a function parameter carries an implicit ENTRY definition, so an
 * explicit conditional assignment is never its "single def".
 *
 * ir_opt_build_def_count / ir_opt_du_build counted only defining IR
 * instructions; a stack-passed param overwritten on one path
 *
 *     int emit(..., int kind, unsigned bs)   // bs = 7th arg, stack-passed
 *     {
 *       if (bs != 0 && kind == 7) bs = 0;    // the only *instruction* def
 *       ...
 *       if (bs != 0) return g(bs);           // folded: "bs is always 0"
 *
 * looked single-def to cmp_expr_fold, which folded every compare after the
 * assign against #0 — deleting the g() branch entirely at -O1/-O2.  In the
 * on-device tcc this hit thumb_emit_data_processing_mop32's barrel_shift
 * parameter: every shift-fused ADD/EOR was emitted WITHOUT its barrel shift
 * (`add.w r1, r2, r0, lsl #6` became `adds r1, r2, r0`), breaking most -O2
 * compiled programs' arithmetic.
 */

#include <stdio.h>

volatile int va[5];
volatile int vkind, vbs;

int sink;
int g(unsigned bs) { sink += (int)bs; return 2; }
int h(void) { return 1; }

int emit(int a1, int a2, int a3, int a4, int a5, int kind, unsigned bs)
{
  if (bs != 0 && kind == 7)
    bs = 0;
  if (a1 == 1 && bs == 0 && kind == 2)
    return h();
  if (bs != 0)
    return g(bs);
  return h();
}

static int run(int a1, int kind, unsigned bs)
{
  va[0] = a1; vkind = kind; vbs = (int)bs;
  return emit(va[0], va[1], va[2], va[3], va[4], vkind, (unsigned)vbs);
}

int main(void)
{
  int r;
  r = run(0, 3, 0x45u); printf("r1=%d sink=%d\n", r, sink);
  r = run(0, 7, 0x45u); printf("r2=%d sink=%d\n", r, sink);
  r = run(1, 2, 0u);    printf("r3=%d sink=%d\n", r, sink);
  r = run(1, 2, 9u);    printf("r4=%d sink=%d\n", r, sink);
  return 0;
}
