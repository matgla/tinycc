/* ssa:const_string_fold — SSA-phase folding of const-string builtins.
 *
 * Exercises the source/opt/ssa/const_string_fold.c pass end-to-end at runtime:
 *   - strlen of a constant string folds to an immediate (str_strlen.c).
 *   - strcmp / memcmp of constants fold to an immediate (str_strcmp.c).
 *   - strcpy(stack, "literal") folds to an inline BLOCK_COPY when (len+1) % 4 == 0
 *     (str_strcpy.c); odd lengths stay a __tcc_strcpy call.  Relies on string
 *     literals being word-aligned (tccgen) so the BLOCK_COPY LDM source is aligned.
 * All results are printed so a miscompiled fold changes the output.
 */
#include <stdio.h>
#include <string.h>

volatile int vsel = 1;

int main(void)
{
  char a[8];
  char b[8];
  char c[8];

  strcpy(a, "abc");      /* len 3, +1=4  -> one-word BLOCK_COPY */
  strcpy(b, "1234567");  /* len 7, +1=8  -> two-word BLOCK_COPY */
  strcpy(c, "xy");       /* len 2, +1=3  -> stays __tcc_strcpy  */

  int l = (int)strlen("hello");           /* -> 5 */
  int e = strcmp("abc", "abc") == 0;      /* -> 1 */
  int n = strcmp("abc", "abd") < 0;       /* -> 1 */
  int m = memcmp("abcd", "abce", 3) == 0; /* -> 1 (first 3 equal) */

  printf("%s|%s|%s|%d|%d|%d|%d\n", a, b, c, l, e, n, m);
  return vsel - 1;
}
