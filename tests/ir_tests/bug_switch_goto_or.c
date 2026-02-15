/* Bug: switch with goto to a common label, where a variable is set
 * to a large constant (e.g. 0x4000) before the goto, and then OR'ed
 * into another variable at the label.
 *
 * Reproduces the parse_btype bug where:
 *   case TOK_TYPEDEF: g = VT_TYPEDEF; goto storage;
 *   storage: t |= g;
 * produces t=3 instead of t=0x4003 on native ARM compilation.
 *
 * The function has many local variables to ensure 'g' is placed at a
 * large negative frame offset (similar to parse_btype's [r7, #-144]).
 */
#include <stdio.h>

/* Use volatile to prevent the compiler from optimizing away the padding */
static volatile int sink;

/* Simulate parse_btype's switch/goto/OR pattern with a large stack frame */
static int test_switch_goto_or(int tok)
{
  /* Many local variables to push 'g' to a large stack offset,
   * similar to parse_btype which has type1, ad1, btype, etc. */
  int pad0 = 0, pad1 = 0, pad2 = 0, pad3 = 0;
  int pad4 = 0, pad5 = 0, pad6 = 0, pad7 = 0;
  int pad8 = 0, pad9 = 0, pad10 = 0, pad11 = 0;
  int pad12 = 0, pad13 = 0, pad14 = 0, pad15 = 0;
  int pad16 = 0, pad17 = 0, pad18 = 0, pad19 = 0;
  int pad20 = 0, pad21 = 0, pad22 = 0, pad23 = 0;
  int t = 3; /* VT_INT */
  int g;

  /* Use the padding to prevent optimization */
  sink = pad0 + pad1 + pad2 + pad3 + pad4 + pad5 + pad6 + pad7;
  sink = pad8 + pad9 + pad10 + pad11 + pad12 + pad13 + pad14 + pad15;
  sink = pad16 + pad17 + pad18 + pad19 + pad20 + pad21 + pad22 + pad23;

  switch (tok)
  {
  case 271: /* TOK_EXTERN */
    g = 0x1000;
    goto storage;
  case 272: /* TOK_STATIC */
    g = 0x2000;
    goto storage;
  case 307: /* TOK_TYPEDEF */
    g = 0x4000;
    goto storage;
  storage:
    if (t & 0x7000 & ~g)
    {
      printf("ERROR: multiple storage classes\n");
      return -1;
    }
    t |= g;
    break;
  default:
    break;
  }

  return t;
}

/* Simpler version without large stack frame for comparison */
static int test_switch_goto_or_simple(int tok)
{
  int t = 3;
  int g;

  switch (tok)
  {
  case 1:
    g = 0x1000;
    goto label;
  case 2:
    g = 0x2000;
    goto label;
  case 3:
    g = 0x4000;
    goto label;
  label:
    t |= g;
    break;
  default:
    break;
  }

  return t;
}

/* Test OR with specific large constants */
static int test_or_const(int base, int val)
{
  int result = base;
  result |= val;
  return result;
}

int main(void)
{
  int result;
  int pass = 1;

  /* Test 1: switch/goto/OR with large stack frame - TOK_EXTERN */
  result = test_switch_goto_or(271);
  printf("extern:  t=0x%x (expect 0x1003)\n", result);
  if (result != 0x1003) { printf("FAIL\n"); pass = 0; }

  /* Test 2: switch/goto/OR with large stack frame - TOK_STATIC */
  result = test_switch_goto_or(272);
  printf("static:  t=0x%x (expect 0x2003)\n", result);
  if (result != 0x2003) { printf("FAIL\n"); pass = 0; }

  /* Test 3: switch/goto/OR with large stack frame - TOK_TYPEDEF */
  result = test_switch_goto_or(307);
  printf("typedef: t=0x%x (expect 0x4003)\n", result);
  if (result != 0x4003) { printf("FAIL\n"); pass = 0; }

  /* Test 4: simple version - case 1 (0x1000) */
  result = test_switch_goto_or_simple(1);
  printf("simple1: t=0x%x (expect 0x1003)\n", result);
  if (result != 0x1003) { printf("FAIL\n"); pass = 0; }

  /* Test 5: simple version - case 2 (0x2000) */
  result = test_switch_goto_or_simple(2);
  printf("simple2: t=0x%x (expect 0x2003)\n", result);
  if (result != 0x2003) { printf("FAIL\n"); pass = 0; }

  /* Test 6: simple version - case 3 (0x4000) */
  result = test_switch_goto_or_simple(3);
  printf("simple3: t=0x%x (expect 0x4003)\n", result);
  if (result != 0x4003) { printf("FAIL\n"); pass = 0; }

  /* Test 7-9: direct OR with large constants */
  result = test_or_const(3, 0x1000);
  printf("or1000:  r=0x%x (expect 0x1003)\n", result);
  if (result != 0x1003) { printf("FAIL\n"); pass = 0; }

  result = test_or_const(3, 0x2000);
  printf("or2000:  r=0x%x (expect 0x2003)\n", result);
  if (result != 0x2003) { printf("FAIL\n"); pass = 0; }

  result = test_or_const(3, 0x4000);
  printf("or4000:  r=0x%x (expect 0x4003)\n", result);
  if (result != 0x4003) { printf("FAIL\n"); pass = 0; }

  result = test_or_const(3, 0x8000);
  printf("or8000:  r=0x%x (expect 0x8003)\n", result);
  if (result != 0x8003) { printf("FAIL\n"); pass = 0; }

  if (pass)
    printf("ALL PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  return pass ? 0 : 1;
}
