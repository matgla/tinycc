/* Regression: IV strength-reduction corrupted a function call's FUNCPARAMVAL
 * sequence and crashed the backend with "missing FUNCPARAMVAL for call_id=N".
 *
 * An in-place struct-array compaction `arr[j++] = arr[i]` builds two derived
 * induction-variable address expressions (&arr[i], &arr[j]) that feed the
 * hidden struct-copy call.  IV-SR rewrote those address computations into a
 * strength-reduced pointer; its stride/postnop instruction-shift bookkeeping
 * (and the downstream copy-prop that merges the address temp into the pointer)
 * then mis-shifted / dropped instructions, deleting a call's PARAM0 — which the
 * backend's call-site scan reports as a missing FUNCPARAMVAL.
 *
 * Fixed by (a) reporting the real stride-insertion position to the caller's
 * shift bookkeeping, (b) transforming one derived IV per loop per pass, and
 * (c) skipping derived IVs whose address feeds a memory access (the form that
 * the rewrite cannot safely reduce).  init/sink/run are noinline so the calls
 * survive to exercise the bug.
 */
#include <stdio.h>

struct S { int a, b, c, d, e, f, g, h; };

__attribute__((noinline)) static void init(struct S *arr, int n)
{
  for (int i = 0; i < n; i++) { arr[i].a = i; arr[i].g = (i & 1); arr[i].h = i; }
}

__attribute__((noinline)) static int sink(struct S *arr, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++) s += arr[i].a + arr[i].h;
  return s;
}

__attribute__((noinline)) static int run(int n)
{
  struct S arr[16];
  init(arr, n);
  int j = 0;
  for (int i = 0; i < n; i++) {
    if (arr[i].g != 0)
      arr[j++] = arr[i];      /* keeps odd-indexed entries, compacted */
  }
  return sink(arr, j);
}

int main(void)
{
  int r = run(16);            /* kept i in {1,3,..,15}; sum a+h = 2*sum(odd) = 128 */
  printf("r=%d\n", r);
  return (r == 128) ? 0 : 1;
}
