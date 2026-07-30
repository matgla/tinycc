/* ssa:fold  symref+addend +/- #imm  ->  symref+(addend +/- imm).
 *
 * The subtraction/addition forms `&a[i][j] - k` (i,j,k a constant local chain)
 * reduce, once the index chain folds, to `GlobalSym(a)+addend SUB #const` --
 * a constant address that fold_binary now collapses into a plain symref so
 * ssa:const_string_fold can constant-fold the enclosing strlen.  Before the
 * fold these stayed runtime `SUB`s and left a __tcc_strlen call (gcc-execute/
 * strlen-4 test_array_ptr, the subtraction-form sites).
 *
 * Each A() prints its line on mismatch, so a miscompiled address changes the
 * output; the values are the real strlen()s of the multi-dim const array.
 */
#include <stdio.h>
#include <string.h>

typedef char A28[28];
typedef A28 A3_28[3];
typedef A3_28 A2_3_28[2];
static const A2_3_28 a = {
  { "1\00012", "123\0001234", "12345\000123456" },
  { "1234567\00012345678", "123456789\0001234567890", "12345678901\000123456789012" }
};

volatile int v;

#define A(expr, N) \
  ((strlen (expr) == (size_t)(N)) ? (void)0 \
   : (printf ("line %i: strlen(%s)!=%i\n", __LINE__, #expr, N), (void)v))

void test (void)
{
  int i0 = 0, i1 = i0 + 1, i2 = i1 + 1;

  /* subtraction forms: base has a nonzero addend, minus a constant */
  A (*(&a[0][1] - i1), 1);   /* (a+28) - 28 = a[0][0] -> "1"     */
  A (*(&a[0][2] - i2), 1);   /* (a+56) - 56 = a[0][0] -> "1"     */
  A (*(&a[0][2] - i1), 3);   /* (a+56) - 28 = a[0][1] -> "123"   */
  A (*(&a[1][2] - i2), 7);   /* (a+140)-56 = a[1][0] -> "1234567"*/
  A (*(&a[1][1] - i1), 7);   /* (a+112)-28 = a[1][0]             */

  /* addition form with a nonzero base addend */
  A (*(&a[0][0] + i2), 5);   /* a+56 = a[0][2] -> "12345"        */
}

int main (void)
{
  test ();
  printf ("ok\n");
  return 0;
}
