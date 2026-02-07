/*
 * Test for local array decay in ternary expressions.
 * Bug: When using (cond ? arr1 : arr2)[i], local arrays were not
 * properly decayed to pointers. Instead of computing FP+offset for
 * the array address, the code was loading from that address, using
 * the array content as if it were an address.
 */

#include <stdio.h>

int test_ternary_local_arrays(int use_first)
{
  int arr1[4] = {1, 2, 3, 4};
  int arr2[4] = {10, 20, 30, 40};
  int sum = 0;

  /* This ternary with local arrays was broken:
   * The array in the false branch wasn't decaying to a pointer properly */
  for (int i = 0; i < 4; i++)
  {
    sum += (use_first ? arr1 : arr2)[i];
  }
  return sum;
}

int test_ternary_char_arrays(int use_first)
{
  char m1[6] = {3, 4, 5, 6, 7, 8};
  char m2[6] = {30, 40, 50, 60, 70, 80};
  int sum = 0;

  for (int i = 0; i < 6; i++)
  {
    sum += (use_first ? m1 : m2)[i];
  }
  return sum;
}

int main(void)
{
  int result1 = test_ternary_local_arrays(1); /* Should be 1+2+3+4 = 10 */
  int result2 = test_ternary_local_arrays(0); /* Should be 10+20+30+40 = 100 */

  printf("arr1 sum: %d (expected 10)\n", result1);
  printf("arr2 sum: %d (expected 100)\n", result2);

  int result3 = test_ternary_char_arrays(1); /* Should be 3+4+5+6+7+8 = 33 */
  int result4 = test_ternary_char_arrays(0); /* Should be 30+40+50+60+70+80 = 330 */

  printf("m1 sum: %d (expected 33)\n", result3);
  printf("m2 sum: %d (expected 330)\n", result4);

  if (result1 == 10 && result2 == 100 && result3 == 33 && result4 == 330)
  {
    printf("PASS\n");
    return 0;
  }
  else
  {
    printf("FAIL\n");
    return 1;
  }
}
