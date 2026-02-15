/* Bug: switch binary search tree corrupts the default chain in gcase().
 *
 * When a switch has >8 cases, TCC generates a binary search tree.
 * The recursive gcase() returns a JUMP instruction (the fall-through
 * for unmatched values in the left subtree) that is linked into the
 * default chain 'dsym'.  In the right subtree's linear scan, the
 * single-value case code was passing 'dsym' to tcc_ir_codegen_test_gen(),
 * which stored it as the JUMPIF target.  The subsequent tcc_ir_backpatch()
 * followed the chain from that target and patched the left subtree's
 * fall-through JUMP to point to a case body instead of default.
 *
 * Effect: values that don't match any case execute a random case handler
 * instead of reaching default.  In parse_btype, tok='*' (42) got routed
 * to the TOK_REGISTER handler which calls next(), consuming the '*' and
 * causing "invalid type for '__stack'" when parsing va_list.
 *
 * Fix: use -1 instead of dsym in the single-value JUMPIF, matching the
 * original TCC behavior of gsym_addr(gvtst(0, 0), p->ind).
 */
#include <stdio.h>

/* Switch with >8 cases (triggers binary search in gcase).
 * Case values are in the 271-310 range, mimicking parse_btype's token switch.
 * Returns:
 *   case value * 10  for matching cases
 *   -1               for default
 */
static int big_switch(int val)
{
  int result;

  switch (val)
  {
  case 271: result = 2710; break;  /* TOK_EXTERN  */
  case 272: result = 2720; break;  /* TOK_STATIC  */
  case 273: result = 2730; break;  /* TOK_UNSIGNED */
  case 274: result = 2740; break;  /* TOK__Atomic */
  case 275: result = 2750; break;  /* TOK_CONST1  */
  case 276: result = 2760; break;  /* TOK_CONST2  */
  case 277: result = 2770; break;  /* TOK_CONST3  */
  case 281: result = 2810; break;  /* TOK_REGISTER */
  case 282: result = 2820; break;  /* TOK_SIGNED1 */
  case 283: result = 2830; break;  /* TOK_SIGNED2 */
  case 284: result = 2840; break;  /* TOK_SIGNED3 */
  case 285: result = 2850; break;  /* TOK_RESTRICT1 */
  case 296: result = 2960; break;  /* TOK_VOID    */
  case 298: result = 2980; break;  /* TOK_INT     */
  case 304: result = 3040; break;  /* TOK_LONG    */
  case 305: result = 3050; break;  /* TOK_STRUCT  */
  case 306: result = 3060; break;  /* TOK_UNION   */
  case 307: result = 3070; break;  /* TOK_TYPEDEF */
  default:  result = -1;   break;
  }

  return result;
}

/* Same structure with a side-effect counter in each case,
 * so we can detect if a wrong case handler executes. */
static int side_effect_count;

static int big_switch_side_effect(int val)
{
  switch (val)
  {
  case 271: side_effect_count++; return 271;
  case 272: side_effect_count++; return 272;
  case 273: side_effect_count++; return 273;
  case 274: side_effect_count++; return 274;
  case 275: side_effect_count++; return 275;
  case 276: side_effect_count++; return 276;
  case 277: side_effect_count++; return 277;
  case 281: side_effect_count++; return 281;
  case 282: side_effect_count++; return 282;
  case 283: side_effect_count++; return 283;
  case 284: side_effect_count++; return 284;
  case 285: side_effect_count++; return 285;
  case 296: side_effect_count++; return 296;
  case 298: side_effect_count++; return 298;
  case 304: side_effect_count++; return 304;
  case 305: side_effect_count++; return 305;
  case 306: side_effect_count++; return 306;
  case 307: side_effect_count++; return 307;
  default:  return -1;
  }
}

int main(void)
{
  int pass = 1;
  int r;

  /* Test 1: values that match cases should return correctly */
  r = big_switch(271); if (r != 2710) { printf("FAIL case 271: got %d\n", r); pass = 0; }
  r = big_switch(307); if (r != 3070) { printf("FAIL case 307: got %d\n", r); pass = 0; }
  r = big_switch(296); if (r != 2960) { printf("FAIL case 296: got %d\n", r); pass = 0; }
  r = big_switch(282); if (r != 2820) { printf("FAIL case 282: got %d\n", r); pass = 0; }

  /* Test 2: values that DON'T match any case MUST reach default (-1).
   * The bug caused these to execute a random case handler instead. */
  r = big_switch(42);   /* '*' character - the original trigger */
  if (r != -1) { printf("FAIL default 42: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(0);
  if (r != -1) { printf("FAIL default 0: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(100);
  if (r != -1) { printf("FAIL default 100: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(270);  /* one below first case */
  if (r != -1) { printf("FAIL default 270: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(278);  /* gap between 277 and 281 */
  if (r != -1) { printf("FAIL default 278: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(280);  /* another gap value */
  if (r != -1) { printf("FAIL default 280: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(290);  /* gap between 285 and 296 */
  if (r != -1) { printf("FAIL default 290: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(308);  /* one above last case */
  if (r != -1) { printf("FAIL default 308: got %d (expected -1)\n", r); pass = 0; }

  r = big_switch(1000);
  if (r != -1) { printf("FAIL default 1000: got %d (expected -1)\n", r); pass = 0; }

  /* Test 3: side-effect version - no side effects for default path */
  side_effect_count = 0;
  r = big_switch_side_effect(42);
  if (r != -1 || side_effect_count != 0) {
    printf("FAIL side_effect 42: r=%d count=%d (expected r=-1 count=0)\n",
           r, side_effect_count);
    pass = 0;
  }

  side_effect_count = 0;
  r = big_switch_side_effect(278);
  if (r != -1 || side_effect_count != 0) {
    printf("FAIL side_effect 278: r=%d count=%d (expected r=-1 count=0)\n",
           r, side_effect_count);
    pass = 0;
  }

  /* Test 4: side-effect version - side effects for matching cases */
  side_effect_count = 0;
  r = big_switch_side_effect(271);
  if (r != 271 || side_effect_count != 1) {
    printf("FAIL side_effect 271: r=%d count=%d (expected r=271 count=1)\n",
           r, side_effect_count);
    pass = 0;
  }

  if (pass)
    printf("ALL PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  return pass ? 0 : 1;
}
