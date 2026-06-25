/* Counterpart to test_sl_fwd_alias.c: an UNCONDITIONAL store through
 * the inlined-callee pointer must still let the post-call read forward.
 * The fix for the conditional case must not over-invalidate this case
 * (which would regress codegen).  Expected output: PASS. */
#include <stdio.h>

typedef struct { int pos; int status; } S;

static void unconditional_set_zero(S *flags) {
  flags->status = 0;
}

int main(void) {
  S f;
  f.status = 1;
  unconditional_set_zero(&f);
  /* After the unconditional write, status is 0.  This must NOT print FAIL. */
  if (f.status != 0) {
    puts("FAIL");
    return 1;
  }
  puts("PASS");
  return 0;
}
