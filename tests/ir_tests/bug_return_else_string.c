/*
 * Test for return from else block with string literals.
 * Bug: When returning a string literal from an else block,
 * the pointer is corrupted (off by some bytes).
 *
 * Observed:
 *   if (i == 1) return "HELLO"; else return "WORLD";
 *   i=0 returns "ORLD" instead of "WORLD"
 *   i=1 returns "HELLO" correctly
 *
 * Workaround: Use local variable assignment instead of direct return.
 */

#include <stdio.h>
#include <string.h>

/* Bug case: direct return from else */
const char *get_str_direct(int i)
{
  if (i == 1)
  {
    return "HELLO";
  }
  else
  {
    return "WORLD";
  }
}

/* Workaround: assign to local then return */
const char *get_str_local(int i)
{
  const char *result;
  if (i == 1)
  {
    result = "HELLO";
  }
  else
  {
    result = "WORLD";
  }
  return result;
}

/* Multiple if-else chain */
const char *get_str_chain(int i)
{
  if (i == 0)
    return "ZERO";
  if (i == 1)
    return "ONE";
  if (i == 2)
    return "TWO";
  return "OTHER";
}

int main(void)
{
  int errors = 0;

  printf("Testing return from else block...\n\n");

  /* Test direct return */
  printf("get_str_direct:\n");
  const char *s0 = get_str_direct(0);
  const char *s1 = get_str_direct(1);
  printf("  i=0: \"%s\" (expected \"WORLD\")\n", s0);
  printf("  i=1: \"%s\" (expected \"HELLO\")\n", s1);
  if (strcmp(s0, "WORLD") != 0)
  {
    printf("  FAIL: i=0\n");
    errors++;
  }
  if (strcmp(s1, "HELLO") != 0)
  {
    printf("  FAIL: i=1\n");
    errors++;
  }

  /* Test local variable workaround */
  printf("\nget_str_local:\n");
  s0 = get_str_local(0);
  s1 = get_str_local(1);
  printf("  i=0: \"%s\" (expected \"WORLD\")\n", s0);
  printf("  i=1: \"%s\" (expected \"HELLO\")\n", s1);
  if (strcmp(s0, "WORLD") != 0)
  {
    printf("  FAIL: i=0\n");
    errors++;
  }
  if (strcmp(s1, "HELLO") != 0)
  {
    printf("  FAIL: i=1\n");
    errors++;
  }

  /* Test chain of if-returns */
  printf("\nget_str_chain:\n");
  for (int i = 0; i <= 3; i++)
  {
    const char *s = get_str_chain(i);
    const char *expected = (i == 0) ? "ZERO" : (i == 1) ? "ONE" : (i == 2) ? "TWO" : "OTHER";
    printf("  i=%d: \"%s\" (expected \"%s\")\n", i, s, expected);
    if (strcmp(s, expected) != 0)
    {
      printf("  FAIL: i=%d\n", i);
      errors++;
    }
  }

  printf("\n");
  if (errors == 0)
  {
    printf("PASS\n");
    return 0;
  }
  else
  {
    printf("FAIL: %d errors\n", errors);
    return 1;
  }
}
