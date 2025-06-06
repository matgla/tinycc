/* Regression guard for the conditional-body / break loop-rotation extensions
 * in ir/opt_loop_utils.c (try_rotate_loop).
 *
 * Two newly-rotatable shapes of a counted for-loop:
 *
 *  1. Diamond conditional body — `for (i...) if (cond) stmt;` where the body's
 *     forward `if`-skip rejoins before the latch (popcount/parity style).  The
 *     skip JUMPIF's target (= body_end_jmp) must remap to the relocated latch.
 *
 *  2. break-via-fall-through — `for (i...) if (cond) break;` where jump
 *     threading collapsed the break JUMP into a fall-through to the loop exit.
 *     Rotation must INVERT the deciding JUMPIF to target the exit, else the
 *     break silently becomes a continue (ctz/clz/ffs style).
 *
 * Both run at every optimization level; correctness depends on edge values
 * (0, all-ones, single low/high bit, alternating) that stress the rotated
 * back-edge, guard elision, and the inverted break condition. */
#include <stdio.h>

/* Diamond conditional body: count set bits. */
static int popcount32(unsigned x)
{
  int i, c = 0;
  for (i = 0; i < 32; i++)
    if (x & ((unsigned)1 << i))
      c++;
  return c;
}

/* break-via-fall-through: index of lowest set bit, or 32 if none. */
static int ctz32(unsigned x)
{
  int i;
  for (i = 0; i < 32; i++)
    if (x & ((unsigned)1 << i))
      break;
  return i;
}

/* break-via-fall-through, counting down from the top: leading-zero count. */
static int clz32(unsigned x)
{
  int i;
  for (i = 0; i < 32; i++)
    if (x & ((unsigned)1 << (31 - i)))
      break;
  return i;
}

/* Diamond body whose `if` carries a side branch to the loop exit too
 * (early-out plus an accumulate): exercises a body with both an internal
 * skip and an external break in the same rotated loop. */
static int sum_until_neg(const int *a, int n)
{
  int i, s = 0;
  for (i = 0; i < n; i++)
  {
    if (a[i] < 0)
      break;
    if (a[i] & 1)
      s += a[i];
  }
  return s;
}

int main(void)
{
  int ok = 1;

  if (popcount32(0u) != 0) ok = 0;
  if (popcount32(0xffffffffu) != 32) ok = 0;
  if (popcount32(0x1u) != 1) ok = 0;
  if (popcount32(0x80000000u) != 1) ok = 0;
  if (popcount32(0xa5a5a5a5u) != 16) ok = 0;

  if (ctz32(0u) != 32) ok = 0;
  if (ctz32(0x1u) != 0) ok = 0;
  if (ctz32(0x80000000u) != 31) ok = 0;
  if (ctz32(0x40u) != 6) ok = 0;
  if (ctz32(0xa5a5a5a5u) != 0) ok = 0;

  if (clz32(0u) != 32) ok = 0;
  if (clz32(0x80000000u) != 0) ok = 0;
  if (clz32(0x1u) != 31) ok = 0;
  if (clz32(0x00010000u) != 15) ok = 0;

  {
    int t1[] = {2, 3, 5, -1, 9};   /* odd before break: 3 + 5 = 8 */
    int t2[] = {1, 1, 1, 1};       /* all odd, no break: 4 */
    int t3[] = {-7, 1};            /* immediate break: 0 */
    if (sum_until_neg(t1, 5) != 8) ok = 0;
    if (sum_until_neg(t2, 4) != 4) ok = 0;
    if (sum_until_neg(t3, 2) != 0) ok = 0;
  }

  printf("%s\n", ok ? "OK" : "FAIL");
  return 0;
}
