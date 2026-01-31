/*
 * Test for nested ternary with string literal pointers.
 * Bug: TCC generates incorrect code for nested ternary expressions
 * that select between string literals. The resulting pointer can
 * point to wrong memory, causing garbage output.
 *
 * Observed in benchmark output:
 *   Expected: "PASS"
 *   Got:      "ry Pi Ltd" (fragment of "Raspberry Pi Ltd" from SDK)
 */

#include <stdio.h>
#include <string.h>

/* Enum to test against */
typedef enum
{
  STATUS_PASS = 0,
  STATUS_FAIL = 1,
  STATUS_SKIP = 2,
  STATUS_UNKNOWN = 3
} status_t;

/* Simple nested ternary - known problematic pattern */
const char *get_status_ternary(status_t s)
{
  return s == STATUS_PASS ? "PASS" : s == STATUS_FAIL ? "FAIL" : s == STATUS_SKIP ? "SKIP" : "?";
}

/* Alternative using if-else (should work correctly) */
const char *get_status_ifelse(status_t s)
{
  if (s == STATUS_PASS)
    return "PASS";
  if (s == STATUS_FAIL)
    return "FAIL";
  if (s == STATUS_SKIP)
    return "SKIP";
  return "?";
}

/* Test with local variable assignment */
const char *get_status_local(status_t s)
{
  const char *result = s == STATUS_PASS ? "PASS" : s == STATUS_FAIL ? "FAIL" : s == STATUS_SKIP ? "SKIP" : "?";
  return result;
}

/* Simpler two-level ternary */
const char *get_simple_ternary(int val)
{
  return val == 0 ? "ZERO" : val == 1 ? "ONE" : "OTHER";
}

int main(void)
{
  int errors = 0;

  printf("Testing nested ternary string selection...\n\n");

  /* Test get_status_ternary */
  printf("get_status_ternary:\n");
  for (int i = 0; i <= 3; i++)
  {
    const char *s = get_status_ternary((status_t)i);
    const char *expected = (i == 0) ? "PASS" : (i == 1) ? "FAIL" : (i == 2) ? "SKIP" : "?";
    int ok = strcmp(s, expected) == 0;
    printf("  status=%d: got \"%s\", expected \"%s\" -> %s\n", i, s, expected, ok ? "OK" : "FAIL");
    if (!ok)
      errors++;
  }

  /* Test get_status_ifelse */
  printf("\nget_status_ifelse:\n");
  for (int i = 0; i <= 3; i++)
  {
    const char *s = get_status_ifelse((status_t)i);
    const char *expected = (i == 0) ? "PASS" : (i == 1) ? "FAIL" : (i == 2) ? "SKIP" : "?";
    int ok = strcmp(s, expected) == 0;
    printf("  status=%d: got \"%s\", expected \"%s\" -> %s\n", i, s, expected, ok ? "OK" : "FAIL");
    if (!ok)
      errors++;
  }

  /* Test get_status_local */
  printf("\nget_status_local:\n");
  for (int i = 0; i <= 3; i++)
  {
    const char *s = get_status_local((status_t)i);
    const char *expected = (i == 0) ? "PASS" : (i == 1) ? "FAIL" : (i == 2) ? "SKIP" : "?";
    int ok = strcmp(s, expected) == 0;
    printf("  status=%d: got \"%s\", expected \"%s\" -> %s\n", i, s, expected, ok ? "OK" : "FAIL");
    if (!ok)
      errors++;
  }

  /* Test get_simple_ternary */
  printf("\nget_simple_ternary:\n");
  for (int i = 0; i <= 2; i++)
  {
    const char *s = get_simple_ternary(i);
    const char *expected = (i == 0) ? "ZERO" : (i == 1) ? "ONE" : "OTHER";
    int ok = strcmp(s, expected) == 0;
    printf("  val=%d: got \"%s\", expected \"%s\" -> %s\n", i, s, expected, ok ? "OK" : "FAIL");
    if (!ok)
      errors++;
  }

  printf("\n");
  if (errors == 0)
  {
    printf("PASS: All tests passed\n");
    return 0;
  }
  else
  {
    printf("FAIL: %d errors\n", errors);
    return 1;
  }
}
