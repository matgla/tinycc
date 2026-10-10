/* Each nested function's register-allocation pipeline resolved the captured
 * offsets of EVERY entry in the nested-function table -- its siblings too --
 * against its own IR, where those (parent) vreg numbers name something else.
 * `sum`, compiled after `rotate` (which has a local), read b from a's slot. */
#include <stdio.h>

int main(void)
{
  int a = 1, b = 2, c = 3;
  void rotate(void) { int t = a; a = b; b = c; c = t; }
  int sum(void) { return a * 100 + b * 10 + c; }
  printf("%d %d %d sum=%d\n", a, b, c, sum());
  rotate();
  printf("%d %d %d sum=%d\n", a, b, c, sum());

  /* A sibling with a child of its own between two readers. */
  int p = 4, q = 5, r = 6;
  int first(int k) { int u = k * 2, w = u + p; return w + q; }
  int middle(int k) { int inner(int j) { return j + r + p; } int s = inner(k); return s * q; }
  int last(void) { return p * 100 + q * 10 + r; }
  printf("%d %d %d\n", first(1), middle(2), last());
  p = 7;
  r = 9;
  printf("%d %d %d\n", first(1), middle(2), last());
  return 0;
}
