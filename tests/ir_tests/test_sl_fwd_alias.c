/* SL-FWD multi-pred merge alias bug repro.
 *
 * Pattern: caller writes a struct field, then calls a (to-be-inlined) helper
 * that reads + conditionally writes that same field through a pointer, then
 * the caller's continuation reads the field.  Bug: SL-FWD interacts with
 * dead-store-elim across the inlined merge and forwards the pre-call value
 * past the conditional in-callee store.
 *
 * Triggers under -O2 -finline-limit=80 (and at default -O2 with current
 * threshold 60 too — see SL_FWD_FIX_PLAN.md).  Expected output: PASS. */
#include <stdio.h>

typedef struct { int pos; int status; } S;

static void inner(S *flags) {
  if (flags->status == 1) flags->status = 0;
  switch (flags->status) {
    case 0: break;
    default: abort();
  }
}

int main(void) {
  S f;
  f.status = 1;
  inner(&f);
  puts("PASS");
  return 0;
}
