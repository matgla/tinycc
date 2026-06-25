extern int printf(const char *, ...);

/* Regression test for an `if (A || B) <skip>` short-circuit codegen bug.
 *
 * When A is an equality (`j == skip`, emits a `beq`) and B is a relational on
 * a computed value (`a[j] < 0`, emits a `blt`), and both operands short-circuit
 * to the same fall-through target (a `continue` or the empty arm of an
 * if/else), the equality branch was mis-targeted onto B's *conditional* branch
 * instead of the shared continue label.  Reaching B's `blt` via the equality
 * path reuses the equality's flags ("equal", not "less"), so the branch is not
 * taken and the `j == skip` case wrongly falls into the body.
 *
 * Only manifested at -O1+ (an always-on optimize>0 lowering path), so the test
 * is most meaningful at -O1. */

int count_valid(int *a, int n, int skip)
{
  int c = 0;
  for (int j = 0; j < n; j++) {
    if (j == skip || a[j] < 0)
      continue;
    c++;
  }
  return c;
}

int sum_else(int *a, int n, int skip)
{
  int s = 0;
  for (int j = 0; j < n; j++) {
    if (j == skip || a[j] < 0) {
      /* skip */
    } else {
      s += a[j];
    }
  }
  return s;
}

int main()
{
  int a[5] = {10, -1, 20, 30, -2};
  /* skip index 2 (value 20) and the negatives (-1, -2): valid are 10 and 30. */
  printf("count=%d\n", count_valid(a, 5, 2)); /* 2 */
  printf("sum=%d\n", sum_else(a, 5, 2));      /* 10 + 30 = 40 */
  return 0;
}
