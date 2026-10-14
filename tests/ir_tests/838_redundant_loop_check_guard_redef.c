/* The loop guard `i < 10` must not decide the in-body `i < 10` test after the
 * body has changed i (redundant_loop_check). */
#include <stdio.h>
void use(int *);
__attribute__((noinline)) int f(int n)
{
  int arr[2];
  arr[0] = 3;
  arr[1] = 4;
  int c = arr[1];
  int i = n;
  while (i < 10)
  {
    i += 5;
    if (i < 10)
      c++;
    i++;
  }
  use(arr);
  return c;
}
void use(int *p) { (void)p; }
int main(void)
{
  int r = f(0);
  printf("f(0) = %d\n", r);
  if (r != 5)
  {
    printf("FAIL: expected 5\n");
    return 1;
  }
  printf("PASS\n");
  return 0;
}
