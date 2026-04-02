/* Minimal reproducer for ternary miscompilation bug.
 * When compiled with armv8m-tcc, the ternary inside the if-body
 * is constant-folded incorrectly. */
#include <stdio.h>

int main(void)
{
  volatile int tok1 = 533;

  int is_float = (tok1 == 534 || tok1 == 537);
  int is_max = (tok1 == 533 || tok1 == 534 || tok1 == 535);

  int func_tok;
  if (is_max)
    func_tok = is_float ? 550 : 549;
  else
    func_tok = is_float ? 553 : 552;

  printf("is_float=%d is_max=%d func_tok=%d expected=549\n", is_float, is_max, func_tok);

  /* Also test: ternary alone (no enclosing if) */
  int bare = is_float ? 550 : 549;
  printf("bare=%d expected=549\n", bare);

  /* Test with different tok1 values */
  tok1 = 534;
  is_float = (tok1 == 534 || tok1 == 537);
  is_max = (tok1 == 533 || tok1 == 534 || tok1 == 535);
  if (is_max)
    func_tok = is_float ? 550 : 549;
  else
    func_tok = is_float ? 553 : 552;
  printf("tok1=534: is_float=%d is_max=%d func_tok=%d expected=550\n", is_float, is_max, func_tok);

  tok1 = 536;
  is_float = (tok1 == 534 || tok1 == 537);
  is_max = (tok1 == 533 || tok1 == 534 || tok1 == 535);
  if (is_max)
    func_tok = is_float ? 550 : 549;
  else
    func_tok = is_float ? 553 : 552;
  printf("tok1=536: is_float=%d is_max=%d func_tok=%d expected=552\n", is_float, is_max, func_tok);

  return 0;
}
