/* bug_ra_interval_forward_exit_backward_reentry with a computed goto in the
 * function: ra_widen_intervals_by_liveness used to bail on IJUMP, leaving x's
 * linear interval in place (169 instead of 18).  Fix: an IJUMP's successors
 * are every address-taken label. */
int printf(const char *, ...);

__attribute__((noinline)) int f(int a, int b, int c, volatile int *p)
{
  int x, y, z, w;
  void *t = &&L50;
  goto start;
L5:
  y = p[1] * 3; z = p[2] * 5; w = p[3] * 7;
  p[4] = y + z + w; p[5] = y ^ z; p[6] = z - w; p[7] = y * w;
  goto *t;
start:
  x = a * b + c;
  if (c & 1) goto L100;
L50:
  return x + p[0];
L100:
  y = p[8] + a; z = p[9] + b; w = p[10] + c;
  p[11] = y * z; p[12] = z * w; p[13] = y * w; p[14] = y + z + w;
  goto L5;
}

int main(void)
{
  volatile int arr[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  int r = f(3, 4, 5, arr);
  printf("%d\n", r);
  return r != 18;
}
