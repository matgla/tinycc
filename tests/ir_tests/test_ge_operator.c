#include <stdio.h>

/* Test case for TinyCC ARM >= operator bug
 *
 * Bug: The >= operator was returning incorrect results in comparison expressions
 * when used with unsigned 32-bit values.
 *
 * Symptoms:
 * - (6 >= 7) returned non-zero garbage value instead of 0
 * - (8 >= 7) returned 0 instead of 1
 * - (7 >= 7) returned 0 instead of 1
 */

typedef unsigned int u32;

int test_ge_direct(u32 a, u32 b)
{
  return a >= b;
}

int test_ge_workaround(u32 a, u32 b)
{
  /* Workaround: avoid >= by using < */
  if (a < b)
    return 0;
  return 1;
}

int main()
{
  printf("Testing >= operator bug:\n\n");

  /* Test cases */
  struct
  {
    u32 a;
    u32 b;
    int expected;
  } tests[] = {
      {7, 7, 1},                                                     /* equal */
      {6, 7, 0},                                                     /* less than */
      {8, 7, 1},                                                     /* greater than */
      {0, 0, 1},                                                     /* both zero */
      {0, 1, 0},                                                     /* zero vs non-zero */
      {100, 50, 1},       {50, 100, 0}, {0xFFFFFFFF, 0xFFFFFFFF, 1}, /* max value */
      {0xFFFFFFFF, 0, 1},                                            /* max vs zero */
      {0, 0xFFFFFFFF, 0},                                            /* zero vs max */
  };

  int num_tests = sizeof(tests) / sizeof(tests[0]);
  int passed_direct = 0;
  int passed_workaround = 0;

  for (int i = 0; i < num_tests; i++)
  {
    int result_direct = test_ge_direct(tests[i].a, tests[i].b);
    int result_workaround = test_ge_workaround(tests[i].a, tests[i].b);

    printf("Test %d: %u >= %u\n", i, tests[i].a, tests[i].b);
    printf("  Expected: %d\n", tests[i].expected);
    printf("  Direct >=: %d %s\n", result_direct, (result_direct == tests[i].expected) ? "PASS" : "FAIL");
    printf("  Workaround: %d %s\n", result_workaround, (result_workaround == tests[i].expected) ? "PASS" : "FAIL");

    if (result_direct == tests[i].expected)
      passed_direct++;
    if (result_workaround == tests[i].expected)
      passed_workaround++;
  }

  printf("\nResults:\n");
  printf("Direct >= operator: %d/%d tests passed\n", passed_direct, num_tests);
  printf("Workaround method: %d/%d tests passed\n", passed_workaround, num_tests);

  if (passed_direct == num_tests)
  {
    printf("\n✓ The >= operator bug is FIXED!\n");
    return 0;
  }
  else if (passed_workaround == num_tests)
  {
    printf("\n✗ The >= operator bug still exists, but workaround works\n");
    return 1;
  }
  else
  {
    printf("\n✗ Both methods failed - critical bug!\n");
    return 2;
  }
}
