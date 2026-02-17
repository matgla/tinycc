/* Bug: post-increment fusion incorrectly creates STORE_POSTINC instead of
 * LOAD_POSTINC for the pattern:  ch = *p++
 *
 * In parse_number(), the sequence:
 *   q = token_buf;
 *   ch = *p++;
 *   t = ch;
 *   ch = *p++;
 *   *q++ = t;
 *
 * generates a STORE_POSTINC (str.w r1, [r4], #1) where a LOAD_POSTINC
 * (ldrb r1, [r4], #1) is needed. The STR writes the pointer value p+1
 * into *p, corrupting the input string, and reads garbage into ch.
 *
 * This triggers "invalid digit" in parse_number when TCC compiles itself
 * natively on ARM, because hex constants like 0x7fffffff in tccdefs
 * get their first bytes overwritten with a pointer value.
 */
#include <stdio.h>
#include <string.h>

static char token_buf[1024];

/* Mirror parse_number's opening pattern exactly.
 * Large frame from bn[] and double forces register pressure
 * similar to the real function. */
static int test_parse_number_pattern(const char *p)
{
  int b, t, shift, frac_bits, s, exp_val, ch;
  char *q;
  unsigned int bn[2];
  double d;

  /* Prevent optimizer from removing unused locals */
  (void)shift;
  (void)frac_bits;
  (void)s;
  (void)exp_val;
  (void)bn;
  (void)d;

  /* The critical pattern from parse_number */
  q = token_buf;
  ch = *p++;
  t = ch;
  ch = *p++;
  *q++ = t;
  b = 10;

  /* Use all values to prevent dead code elimination */
  if (t == '.')
    b = 0;
  else if (t == '0' && (ch == 'x' || ch == 'X'))
    b = 16;

  /* Pack results for verification */
  return ((unsigned char)token_buf[0] << 24) | ((unsigned char)t << 16) | ((unsigned char)ch << 8) | (b & 0xff);
}

/* Simpler version without large frame for comparison */
static int test_post_inc_simple(const char *p)
{
  int ch, t;
  char buf[4];
  char *q = buf;

  ch = *p++;
  t = ch;
  ch = *p++;
  *q++ = t;

  return ((unsigned char)buf[0] << 16) | ((unsigned char)ch << 8) | (unsigned char)t;
}

/* Test that input string is not corrupted by reading from it */
static int test_no_corruption(char *p)
{
  char *orig = p;
  int ch, t;
  char buf[4];
  char *q = buf;

  ch = *p++;
  t = ch;
  ch = *p++;
  *q++ = t;

  /* Return 1 if input is intact, 0 if corrupted */
  return (orig[0] == '0' && orig[1] == 'x' && orig[2] == '7' && orig[3] == 'f');
}

int main(void)
{
  int pass = 1;
  int r;

  /* Test 1: parse_number pattern with "0x7f" */
  r = test_parse_number_pattern("0x7f");
  {
    int q0 = (r >> 24) & 0xff;
    int t = (r >> 16) & 0xff;
    int ch = (r >> 8) & 0xff;
    int b = r & 0xff;

    if (q0 != '0')
    {
      printf("FAIL test1 token_buf[0]: got 0x%x expected 0x%x ('0')\n", q0, '0');
      pass = 0;
    }
    if (t != '0')
    {
      printf("FAIL test1 t: got 0x%x expected 0x%x ('0')\n", t, '0');
      pass = 0;
    }
    if (ch != 'x')
    {
      printf("FAIL test1 ch: got 0x%x expected 0x%x ('x')\n", ch, 'x');
      pass = 0;
    }
    if (b != 16)
    {
      printf("FAIL test1 b: got %d expected 16\n", b);
      pass = 0;
    }
  }

  /* Test 2: simple post-increment */
  r = test_post_inc_simple("AB");
  {
    int q0 = (r >> 16) & 0xff;
    int ch = (r >> 8) & 0xff;
    int t = r & 0xff;

    if (q0 != 'A')
    {
      printf("FAIL test2 buf[0]: got 0x%x expected 0x%x ('A')\n", q0, 'A');
      pass = 0;
    }
    if (ch != 'B')
    {
      printf("FAIL test2 ch: got 0x%x expected 0x%x ('B')\n", ch, 'B');
      pass = 0;
    }
    if (t != 'A')
    {
      printf("FAIL test2 t: got 0x%x expected 0x%x ('A')\n", t, 'A');
      pass = 0;
    }
  }

  /* Test 3: verify input is not corrupted (the key symptom) */
  {
    char input[] = "0x7f";
    char saved[5];
    memcpy(saved, input, 5);

    r = test_no_corruption(input);
    if (!r)
    {
      printf("FAIL test3 input corrupted: got [0x%x 0x%x 0x%x 0x%x] expected [0x%x 0x%x 0x%x 0x%x]\n",
             (unsigned char)input[0], (unsigned char)input[1], (unsigned char)input[2], (unsigned char)input[3],
             (unsigned char)saved[0], (unsigned char)saved[1], (unsigned char)saved[2], (unsigned char)saved[3]);
      pass = 0;
    }
  }

  /* Test 4: parse_number pattern with "AB" (non-hex) */
  r = test_parse_number_pattern("AB");
  {
    int q0 = (r >> 24) & 0xff;
    int t = (r >> 16) & 0xff;
    int ch = (r >> 8) & 0xff;

    if (q0 != 'A')
    {
      printf("FAIL test4 token_buf[0]: got 0x%x expected 0x%x ('A')\n", q0, 'A');
      pass = 0;
    }
    if (t != 'A')
    {
      printf("FAIL test4 t: got 0x%x expected 0x%x ('A')\n", t, 'A');
      pass = 0;
    }
    if (ch != 'B')
    {
      printf("FAIL test4 ch: got 0x%x expected 0x%x ('B')\n", ch, 'B');
      pass = 0;
    }
  }

  if (pass)
    printf("ALL PASSED\n");
  else
    printf("SOME TESTS FAILED\n");

  return pass ? 0 : 1;
}
