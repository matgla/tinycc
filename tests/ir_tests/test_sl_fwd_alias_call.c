/* Variant: post-store call to an externally-defined function may write
 * through the escaped pointer.  Entry-store-prop / SL-FWD must NOT forward
 * the pre-call value past the call.  Expected output: PASS. */
#include <stdio.h>

typedef struct { int pos; int status; } S;

/* Defined here but with side effects the optimizer cannot prove away. */
void may_write(S *flags) {
  flags->status = 0;
}

int main(void) {
  S f;
  f.status = 1;
  may_write(&f);
  /* Must observe 0, the value written by the callee. */
  if (f.status != 0) {
    puts("FAIL");
    return 1;
  }
  puts("PASS");
  return 0;
}
