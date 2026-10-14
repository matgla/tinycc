/* ra:branch_thread trusted a `vr <- x` copy made before a loop header that is
 * itself the flag setter (`TEST_ZERO t` with t == x via GVN).  The back edge
 * reaches that header without executing the copy, vr holds a later value
 * there, yet every iteration's edge into the join was threaded as if vr == x,
 * skipping the test of vr.  With n == 3 and p[0] == p[1] == 0, only the first
 * iteration enters J (t != 0, vr == 3 -> +1); the later ones have t != 0 too
 * but vr == 0 (+100 each): 201.  Threaded, f returned 3. */
int printf(const char *, ...);

__attribute__((noinline)) int f(int x, int n, volatile int *p)
{
  int vr = x * 3;
  int t = x * 3; /* GVN: the same temp as vr's first value */
  int acc = 0;
  for (;;)
  {
    if (t)
      goto J;
    if (p[0] == 5)
      goto J;
    goto skip;
  J:
    if (vr)
      acc += 1;
    else
      acc += 100;
  skip:
    vr = p[1];
    if (--n == 0)
      break;
  }
  return acc;
}

int main(void)
{
  volatile int arr[2] = {0, 0};
  int r = f(1, 3, arr);
  printf("%d (expect 201)\n", r);
  return r != 201;
}
