/* ssa:arm_fuse_shl_add_to_{load,store}_indexed folded `t = a + (i << 2); *t`
 * into one [a, i, lsl #2] access that reads i (and a) at the memory op.  A
 * reassigned parameter is not renamed in SSA, so when i or a is redefined
 * between the address computation and the access the fused form used the NEW
 * value: the store below hit a[x] instead of a[old i]. */
#include <stdio.h>

__attribute__((noinline)) void store_idx_redef(int *a, int i, int x)
{
  int *q = &a[i];
  i = x;
  *q = 7;
  a[i] += 9;
}

__attribute__((noinline)) int load_idx_redef(int *a, int i, int x)
{
  int *q = &a[i];
  i = x;
  return *q + a[i] * 100;
}

__attribute__((noinline)) void store_base_redef(int *a, int *b, int i)
{
  int *q = &a[i];
  a = b;
  *q = 5;
  a[i] += 3;
}

__attribute__((noinline)) int load_base_redef(int *a, int *b, int i)
{
  int *q = &a[i];
  a = b;
  return *q + a[i] * 100;
}

int A[8], B[8];

int main(void)
{
  store_idx_redef(A, 1, 3);
  printf("%d %d %d %d\n", A[0], A[1], A[2], A[3]);

  A[1] = 4;
  A[3] = 6;
  printf("%d\n", load_idx_redef(A, 1, 3));

  A[1] = 0;
  A[3] = 0;
  store_base_redef(A, B, 1);
  printf("%d %d %d %d\n", A[1], B[1], A[2], B[2]);

  A[1] = 4;
  B[1] = 6;
  printf("%d\n", load_base_redef(A, B, 1));
  return 0;
}
