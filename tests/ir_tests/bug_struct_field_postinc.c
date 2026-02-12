/*
 * Bug: struct field post-increment in for-loop does not actually increment.
 *
 * When a struct field like gof.argc is used as the increment expression
 * in a for loop (gof.argc++), the compiler must:
 *   1. Load the field value from the struct's address
 *   2. Add 1
 *   3. Store the result back through the struct's address
 *
 * The bug occurs when the address vreg (pointing to the struct field)
 * gets spilled by the register allocator. The store then writes the
 * incremented value to the spill slot itself rather than through the
 * pointer stored in the spill slot, so the struct field is never updated.
 *
 * Root cause: tcc_ir_materialize_addr_ir() does not handle the VT_LLOCAL
 * case (is_llocal=1) for spilled pointer lvalues, causing the store to
 * fall through to a plain FP-relative store instead of a two-step
 * "load pointer from spill, then store through pointer" sequence.
 */

#include <stdio.h>

struct loop_state
{
  int argc;
  int minargs;
  int maxargs;
  char *arg;
};

/* Use volatile to prevent the compiler from optimizing away the loop */
volatile int sink;

int main(void)
{
  struct loop_state gof;
  int items[] = {10, 20, 30, 0}; /* sentinel-terminated */

  gof.argc = 0;
  gof.minargs = 0;
  gof.maxargs = 4;

  /* This for-loop increment (gof.argc++) is the bug trigger.
   * The struct field address gets spilled under register pressure,
   * and the post-increment store writes to the wrong location. */
  for (gof.argc = 0; items[gof.argc] != 0; gof.argc++)
  {
    sink = items[gof.argc];
  }

  printf("argc=%d\n", gof.argc);

  /* Also test with a simple counting loop */
  for (gof.argc = 1; gof.argc <= 5; gof.argc++)
  {
    sink = gof.argc;
  }

  printf("argc=%d\n", gof.argc);

  return 0;
}
