/*
 * Bug: TinyCC clobbers function pointer when passed as 5th argument.
 *
 * When a function pointer is passed as the 5th parameter (i.e. via the stack
 * on ARM), TinyCC loads it into r1, then overwrites r1 with the 2nd call
 * argument before using r1 as the indirect branch target.
 *
 * Reduced from musl libc's qsort where fix(a, root, n, sz, cmp) calls
 * cmp(a + max * sz, a + (max + 1) * sz) and the blx target becomes a
 * data pointer instead of the comparator.
 */
#include <stdio.h>

int my_cmp(const void *a, const void *b)
{
  int va = *(const int *)a;
  int vb = *(const int *)b;
  if (va < vb)
    return -1;
  if (va > vb)
    return 1;
  return 0;
}

/* 5 parameters: the function pointer is the 5th, passed on the stack */
int call_fifth(int a, int b, int c, int d, int (*fn)(const void *, const void *))
{
  return fn(&a, &b);
}

/* Same bug in a loop context (closer to the original qsort/fix pattern) */
void fix(char *a, int root, int n, int sz, int (*cmp)(const void *, const void *))
{
  while (2 * root <= n)
  {
    int max = 2 * root;
    if (max < n && cmp(a + max * sz, a + (max + 1) * sz) < 0)
      max++;
    if (max && cmp(a + root * sz, a + max * sz) < 0)
    {
      /* swap */
      int tmp;
      char *p = a + root * sz;
      char *q = a + max * sz;
      for (int i = 0; i < sz; i++)
      {
        tmp = p[i];
        p[i] = q[i];
        q[i] = tmp;
      }
      root = max;
    }
    else
    {
      break;
    }
  }
}

int main()
{
  int x = 10, y = 20;

  /* Test 1: simple 5th-arg function pointer call */
  int r = call_fifth(x, y, 0, 0, my_cmp);
  printf("call_fifth(10, 20): %d\n", r);

  /* Test 2: heapsort fix with 5th-arg comparator (qsort pattern) */
  int arr[] = {30, 10, 20, 40};
  int n = 4;

  /* Build max-heap */
  for (int i = (n + 1) / 2; i > 0; i--)
    fix((char *)arr, i - 1, n - 1, sizeof(int), my_cmp);

  printf("After heapify: %d %d %d %d\n", arr[0], arr[1], arr[2], arr[3]);

  /* Heap property: arr[0] should be the maximum */
  if (arr[0] == 40)
    printf("Heap OK: max=%d\n", arr[0]);
  else
    printf("Heap FAIL: expected max=40, got %d\n", arr[0]);

  return 0;
}
