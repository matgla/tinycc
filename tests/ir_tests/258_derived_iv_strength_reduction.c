#include <stdarg.h>
#include <stdio.h>

/*
 * Derived-IV strength reduction regression test (docs/bugs.md #2).
 *
 * Covers both halves of the bug #2 re-enable:
 *
 * 1. va-arg-24 reduction (varargs_fill9 / varargs_fill3): an array store in
 *    a va_arg loop.  The DIV's address temp `T = &n[0] + (i<<2)` reaches the
 *    loop's STORE through a chain the old feeds_mem scan could not see (the
 *    loop body is a detached range after the back-edge), so the DIV was
 *    transformed; the single-trip variant (varargs_fill9: i = 10..10) then
 *    produced `CMP ptr,end` with provably-equal stack offsets at a back-edge
 *    target, which tcc_ir_opt_cmp_stack_addr_fold resolved through the
 *    preheader init only (missing the loop-carried `ptr += 4`) and folded the
 *    loop's ONLY exit test away — infinite loop, HardFault.  Now the escape
 *    scan skips memory-feeding DIVs entirely, and the CMP fold refuses to
 *    resolve vregs at merge points.
 *
 * 2. Register-only derived IV (addr_sum / addr_sum_single): the address value
 *    is only accumulated, never dereferenced — the transform SHOULD fire and
 *    must keep the arithmetic exact.  addr_sum_single is the single-trip
 *    shape whose post-transform `CMP ptr,end` has equal offsets — the exact
 *    pattern the resolver fix keeps sound.  opaque() blocks const-sim/unroll
 *    so a real runtime loop remains at -O1/-O2.
 */

static int errors = 0;

static void
verify (const char *tcase, int *n, int count)
{
  int i;
  for (i = 0; i < count; i++)
    if (n[i] != i)
      {
        printf ("%s: n[%d] = %d expected %d\n", tcase, i, n[i], i);
        errors++;
      }
}

static void
varargs_fill9 (int q0, int q1, int q2, int q3, int q4, int q5, int q6,
               int q7, int q8, int q9, ...)
{
  va_list ap;
  int n[11];
  int i;

  va_start (ap, q9);
  n[0] = q0; n[1] = q1; n[2] = q2; n[3] = q3; n[4] = q4;
  n[5] = q5; n[6] = q6; n[7] = q7; n[8] = q8; n[9] = q9;
  for (i = 9 + 1; i <= 10; i++)   /* single trip: ptr init == end pointer */
    n[i] = va_arg (ap, int);
  va_end (ap);

  verify ("varargs_fill9", n, 11);
}

static void
varargs_fill3 (int q0, int q1, int q2, int q3, ...)
{
  va_list ap;
  int n[11];
  int i;

  va_start (ap, q3);
  n[0] = q0; n[1] = q1; n[2] = q2; n[3] = q3;
  for (i = 3 + 1; i <= 10; i++)   /* multi trip */
    n[i] = va_arg (ap, int);
  va_end (ap);

  verify ("varargs_fill3", n, 11);
}

/* Opaque call through a volatile function pointer: cannot be inlined, and a
 * real CALL in the loop body keeps loop_const_sim/loop_unroll from folding
 * the loops below, so a genuine runtime loop with a derived IV survives to
 * IV/SR. */
static int opaque_counter = 0;
static void
opaque_impl (void)
{
  opaque_counter++;
}
static void (*volatile opaque) (void) = opaque_impl;

/* Register-only derived IV: &arr[i] is accumulated, never dereferenced —
 * the transform SHOULD fire (base is a stack address, value stays in
 * registers).  sum(&arr[0..9]) - 10*&arr[0] == 4 * (0+1+...+9) == 180
 * regardless of where the array lands. */
static unsigned
addr_sum (void)
{
  int arr[16];
  unsigned s = 0;
  int i;
  for (i = 0; i <= 9; i++)
    {
      s += (unsigned) &arr[i];
      opaque ();
    }
  return s - 10u * (unsigned) &arr[0];
}

/* Single-trip variant: after strength reduction + IV elimination the exit
 * test is `CMP ptr, end` where BOTH sides resolve to the same stack offset
 * on the entry path — must NOT be constant-folded (the CMP sits at the
 * back-edge merge; ptr is redefined in the loop). */
static unsigned
addr_sum_single (void)
{
  int arr[16];
  unsigned s = 0;
  int i;
  for (i = 10; i <= 10; i++)
    {
      s += (unsigned) &arr[i];
      opaque ();
    }
  return s - (unsigned) &arr[0];
}

int
main (void)
{
  varargs_fill9 (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  varargs_fill3 (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);

  printf ("addr_sum: %u\n", addr_sum ());
  printf ("addr_sum_single: %u\n", addr_sum_single ());
  printf ("opaque: %d\n", opaque_counter);
  printf ("errors: %d\n", errors);

  return errors ? 1 : 0;
}
