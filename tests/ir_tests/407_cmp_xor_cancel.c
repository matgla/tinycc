/* Guard: `(x ^ y) cmp y` -> `x cmp 0`, valid for == and != only.
 *
 * The rewrite preserves Z but not N/C/V, so it must fire for equality and
 * stay away from the relational predicates: `(x^y) < y` is unrelated to
 * `x < 0`.  Every shape below is checked against the value the unfolded
 * expression must produce, over operand pairs chosen to straddle sign
 * boundaries and the unsigned wrap point — exactly where a predicate that
 * leaked through the gate would disagree.
 *
 * Also covered: both operand orders, the XOR on either side of the compare,
 * a store between the XOR and the compare (which must stop the fold, since
 * the cancelled operand is re-read at the compare), and a volatile operand.
 */
#include <stdio.h>

static int fails;

static void expect(const char *what, int got, int want)
{
  if (got != want)
  {
    printf("FAIL %s: got %d want %d\n", what, got, want);
    fails++;
  }
}

__attribute__((noinline)) static int eq_rhs(int x, int y) { return (x ^ y) == y; }
__attribute__((noinline)) static int eq_lhs(int x, int y) { return y == (x ^ y); }
__attribute__((noinline)) static int ne_rhs(int x, int y) { return (x ^ y) != y; }
/* cancelling term is the XOR's first operand */
__attribute__((noinline)) static int eq_first(int x, int y) { return (y ^ x) == y; }

/* relational: must NOT be rewritten to `x < 0` etc. */
__attribute__((noinline)) static int lt(int x, int y) { return (x ^ y) < y; }
__attribute__((noinline)) static int ge(int x, int y) { return (x ^ y) >= y; }
__attribute__((noinline)) static int ult(unsigned x, unsigned y) { return (x ^ y) < y; }
__attribute__((noinline)) static int ugt(unsigned x, unsigned y) { return (x ^ y) > y; }

/* branch form (JUMPIF consumer) rather than a materialised 0/1 */
__attribute__((noinline)) static int eq_branch(int x, int y)
{
  if ((x ^ y) == y)
    return 11;
  return 22;
}

/* through memory: the compare re-reads *q, so the loads must be provably the
 * same value.  With a store to *p in between, folding away the `*q` read
 * would be fine, but the surviving `*p` read moves down past the store — the
 * fold must not change what this returns. */
__attribute__((noinline)) static int mem_store_between(int *p, int *q, int *w)
{
  int t = *p ^ *q;
  *w = 0x1234;
  return t == *q;
}

/* aliasing: w == p, so the store changes the value the fold would re-read */
__attribute__((noinline)) static int mem_alias(int *p, int *q)
{
  int t = *p ^ *q;
  *p = 0;
  return t == *q;
}

__attribute__((noinline)) static int vol_operand(volatile int *p, int y)
{
  int a = *p;
  return (a ^ y) == y;
}

int main(void)
{
  static const int xs[] = {0, 1, -1, 5, -5, 0x7fffffff, (-0x7fffffff - 1)};
  static const int ys[] = {0, 1, -1, 7, -7, 0x7fffffff, (-0x7fffffff - 1)};
  int i, j;

  for (i = 0; i < (int)(sizeof(xs) / sizeof(xs[0])); i++)
    for (j = 0; j < (int)(sizeof(ys) / sizeof(ys[0])); j++)
    {
      int x = xs[i], y = ys[j];
      unsigned ux = (unsigned)x, uy = (unsigned)y;
      expect("eq_rhs", eq_rhs(x, y), (x ^ y) == y);
      expect("eq_lhs", eq_lhs(x, y), y == (x ^ y));
      expect("ne_rhs", ne_rhs(x, y), (x ^ y) != y);
      expect("eq_first", eq_first(x, y), (y ^ x) == y);
      expect("lt", lt(x, y), (x ^ y) < y);
      expect("ge", ge(x, y), (x ^ y) >= y);
      expect("ult", ult(ux, uy), (ux ^ uy) < uy);
      expect("ugt", ugt(ux, uy), (ux ^ uy) > uy);
      expect("eq_branch", eq_branch(x, y), ((x ^ y) == y) ? 11 : 22);
    }

  {
    int a = 0, b = 9, w = 0;
    expect("mem_zero", mem_store_between(&a, &b, &w), 1);
    expect("mem_w", w, 0x1234);
    a = 3;
    expect("mem_nonzero", mem_store_between(&a, &b, &w), 0);

    a = 0;
    b = 9;
    /* t = 0^9 = 9; *p=0 does not touch *q; 9 == 9 -> 1 */
    expect("mem_alias_zero", mem_alias(&a, &b), 1);
    a = 4;
    b = 9;
    /* t = 4^9 = 13; 13 == 9 -> 0 */
    expect("mem_alias_nonzero", mem_alias(&a, &b), 0);
  }

  {
    int v = 0;
    expect("vol_zero", vol_operand(&v, 6), 1);
    v = 2;
    expect("vol_nonzero", vol_operand(&v, 6), 0);
  }

  printf("fails=%d\n", fails);
  return 0;
}
