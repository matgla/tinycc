/* Register-allocator live interval bug: x is live on a path that leaves its
 * linear [def, last use] range by a FORWARD jump (to L100, placed after x's
 * last use) and re-enters by a BACKWARD jump (L5, placed before x's def), then
 * jumps into the use at L50.  Neither end of the excursion lies inside x's
 * interval, so L100/L5 temporaries used to get x's register.  Wrong at every
 * -O level (169 at -O1/-O2/-Os, 7 at -O0); correct is 18. */
int printf(const char *, ...);

__attribute__((noinline)) int f(int a, int b, int c, volatile int *p)
{
  int x, y, z, w;
  goto start;
L5: /* placed before x's def, reached only from L100 */
  y = p[1] * 3; z = p[2] * 5; w = p[3] * 7;
  p[4] = y + z + w; p[5] = y ^ z; p[6] = z - w; p[7] = y * w;
  goto L50;
start:
  x = a * b + c;
  if (c & 1) goto L100;   /* forward exit past x's last use */
L50:
  return x + p[0];
L100:
  y = p[8] + a; z = p[9] + b; w = p[10] + c;
  p[11] = y * z; p[12] = z * w; p[13] = y * w; p[14] = y + z + w;
  goto L5;                /* backward re-entry before x's def */
}

int main(void)
{
  volatile int arr[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  int r = f(3, 4, 5, arr);
  printf("%d (expect 18)\n", r);
  return r != 18;
}
