/* nested_capture_array.c — Phase 2: Capture array from parent */
#include <stdio.h>

int main(void)
{
  int arr[5] = {10, 20, 30, 40, 50};

  int get(int i)
  {
    return arr[i];
  }
  void set(int i, int v)
  {
    arr[i] = v;
  }

  printf("%d %d %d\n", get(0), get(2), get(4));
  set(2, 99);
  printf("%d\n", get(2));
  printf("%d\n", arr[2]);
  return 0;
}
