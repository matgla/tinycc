/* The address of an element in a static initializer: under nocode_wanted
 * indir() folded the element's load into its value and '&' rebuilt the lvalue
 * from the symbol alone, so `&nums[2]` became `&nums` (offset 0, and a pointer
 * to the whole array: "assignment from incompatible pointer type").  The fold
 * also read a relocated word's bytes as its value: a pointer element folded to
 * its addend. */
#include <stdio.h>

static int nums[] = {7, 8, 9};
static const int cnums[] = {17, 18, 19};
static int m[2][3] = {{1, 2, 3}, {4, 5, 6}};
static struct
{
  int a, b;
} s = {30, 31};
static int x = 40, y = 41;
static int *const ptrs[] = {&x, &y};

int *q1 = &nums[2];
int *q2 = &nums[1] + 1;
const int *q3 = &cnums[1];
int *q4 = &s.b;
int *q5 = &m[1][2];
int *const *q6 = &ptrs[1];
static int *const *const q7 = &ptrs[1];

int main(void)
{
  printf("%d %d %d %d %d\n", *q1, *q2, *q3, *q4, *q5);
  printf("%d %d\n", (int)(q5 - &m[0][0]), (int)(q1 - nums));
  printf("%d %d\n", **q6, **q7);
  return 0;
}
