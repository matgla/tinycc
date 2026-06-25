/* Variant: caller initializes two struct fields at distinct offsets.
 * The inlined callee writes only one of them via a pointer.  After the
 * inlined call, the un-touched field's entry-store value MUST still
 * forward (no over-invalidation regression), and the touched field's
 * post-call read must observe the new value.  Expected output: PASS. */
#include <stdio.h>

typedef struct { int pos; int status; } S;

static void touch_status_only(S *flags) {
  flags->status = 7;
}

int main(void) {
  S f;
  f.pos = 42;
  f.status = 1;
  touch_status_only(&f);
  if (f.status != 7) { puts("FAIL status"); return 1; }
  if (f.pos != 42)   { puts("FAIL pos");    return 1; }
  puts("PASS");
  return 0;
}
