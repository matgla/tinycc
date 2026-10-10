/* Companion of bug_ra_interval_forward_exit_backward_reentry: x is live-in at
 * L5 (forward exit to L100, backward re-entry) and L5's FIRST instruction is
 * an asm statement clobbering r0-r3/r12.  The widened range begins exactly at
 * that instruction, and the call/asm crossing tests exclude a range's start,
 * so x was left in a caller-saved register (78 instead of 18 at every -O). */
int printf(const char *, ...);
__attribute__((noinline)) int f(int a, int b, int c, volatile int *p)
{
  int x, y;
  goto start;
L5:
  __asm__ volatile("mov r0, #77\n mov r1, #77\n mov r2, #77\n mov r3, #77\n mov r12, #77" ::: "r0","r1","r2","r3","r12","memory");
  y = p[1] * 3;
  p[4] = y;
  goto L50;
start:
  x = a * b + c;
  if (c & 1) goto L100;
L50:
  return x + p[0];
L100:
  y = p[8] + a;
  p[11] = y;
  goto L5;
}
int main(void)
{
  volatile int arr[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
  int r = f(3, 4, 5, arr);
  printf("%d (expect 18)\n", r);
  return r != 18;
}
