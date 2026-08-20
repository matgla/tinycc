/* Post-RA copy passes must honor phi_pinned intervals.
 *
 * The clamp diamond `int e = bi; if (e >= size) e = size - 1;` followed by a
 * `for (k = t; k <= e; k++)` loop renames e into a phi web (e1 = bi on one
 * arm, e2 = size-1 on the other, e3 at the join).  When the allocator gives
 * e1 and e3 the same register, post_ra_forward_diamond deletes the identity
 * phi copy and threads the branch straight to the join — pinning BOTH
 * intervals (phi_pinned) because the share is now load-bearing: e1's def is
 * the only write that puts bi's value in the shared register on that path.
 *
 * ra_copy_propagate then saw `e1 <- bi; CMP e1, size` with e1's interval
 * ending at the compare, forwarded the read, and deleted e1's def — blind to
 * the pin.  The shared register entered the loop holding whatever it last
 * held (here: the rows POINTER, loaded for the null check), so the loop
 * bound became an address and the or-loop ran off the end of memory.  On
 * device this HardFaulted the self-hosted tcc in ra_refine_live_regs_accurate
 * while compiling ANY loop at any -O level (04_for.c, 09_do_while.c).
 *
 * The fusion of compared array elements (cdc3d912) is what first produced
 * the shape; the latent blindness was ra_copy_propagate's (and its mirror,
 * ra_retarget_producer's).  Both now refuse phi_pinned endpoints.
 */
#include <stdio.h>

typedef struct {
  unsigned *rows;
  int size;
} LS;

typedef struct {
  int n;
  LS ls;
} IR;

__attribute__((noinline)) static void mark_loops(IR *ir, int *targets, unsigned *masks)
{
  for (int bi = 0; bi < ir->n; bi++) {
    int t = targets[bi];
    if (t < 0 || t >= bi)
      continue;
    unsigned mask = masks[bi] & 0x1FFFu;
    if (!mask || !ir->ls.rows)
      continue;
    int e = bi;
    if (e >= ir->ls.size)
      e = ir->ls.size - 1;
    for (int k = t; k <= e; k++)
      ir->ls.rows[k] |= mask;
  }
}

int main(void)
{
  unsigned rows[8] = {0};
  int targets[8] = {-1, 0, 9, 1, -1, 2, 5, 3};
  unsigned masks[8] = {1, 2, 4, 8, 16, 32, 64, 0x2000};
  IR ir;
  ir.n = 8;
  ir.ls.rows = rows;
  ir.ls.size = 5; /* smaller than n so the clamp arm runs for bi >= 5 */

  mark_loops(&ir, targets, masks);

  unsigned sum = 0;
  for (int i = 0; i < 8; i++)
    sum = sum * 31 + rows[i];
  printf("sum=%08x\n", sum);
  printf("rows=%u %u %u %u %u %u %u %u\n",
         rows[0], rows[1], rows[2], rows[3],
         rows[4], rows[5], rows[6], rows[7]);
  return 0;
}
