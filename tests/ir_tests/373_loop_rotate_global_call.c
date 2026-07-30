/* Loop rotation coverage: global-base bodies and noreturn-call bodies.
 *
 * Two rotation blockers removed together:
 *  - a global/static array base kept the body access as ADD+DEREF (the
 *    unscaled indexed fusion required two vreg operands), tripping the
 *    indirect-lvalue rotation guard;
 *  - any call in the body was rejected, including the `if (v[i] != K)
 *    abort();` check-loop shape where the callee is noreturn.
 *
 * These runtime checks pin the semantics: iteration counts, early aborts NOT
 * taken, boundary indices, IV values live after the loop, and stores through
 * fused global-indexed addresses.  Miscompiled rotation shows up as a wrong
 * sum, a wrong final IV, or a spurious abort. */

extern int printf(const char *, ...);
extern void abort(void);
extern void exit(int);

unsigned char gv[88];
int gw[24];
volatile int sink;

static void __attribute__((noinline)) fill(void)
{
  int i;
  for (i = 0; i < 88; i++)
    gv[i] = 0xaa;
  for (i = 0; i < 24; i++)
    gw[i] = i * 3;
}

/* global byte base, constant limit — must fuse (ldrb [rb, ri]) and rotate */
static int __attribute__((noinline)) sum_g8(void)
{
  int s = 0;
  for (int i = 0; i < 8; i++)
    s += gv[i];
  return s;
}

/* global byte base, runtime limit — rotation with guard */
static int __attribute__((noinline)) sum_gn(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += gv[i];
  return s;
}

/* global int base, scaled index */
static int __attribute__((noinline)) sum_gw(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    s += gw[i];
  return s;
}

/* store through fused global-indexed address */
static void __attribute__((noinline)) set_range(int lo, int hi, unsigned char val)
{
  for (int i = lo; i < hi; i++)
    gv[i] = val;
}

/* the memclr check-loop shape: noreturn call in the body, IV live across
 * sequential loops */
static int __attribute__((noinline)) check_ranges(int a, int b)
{
  int i;
  for (i = 0; i < a; i++)
    if (gv[i] != 0x55)
      abort();
  for (; i < b; i++)
    if (gv[i] != 0xaa)
      abort();
  return i;
}

/* diamond body with a store (no call) over a global — rotation + fusion */
static int __attribute__((noinline)) count_and_flag(int n, unsigned char needle)
{
  int hits = 0;
  for (int i = 0; i < n; i++)
    if (gv[i] == needle)
      hits++;
    else
      sink = i;
  return hits;
}

/* zero-trip: the guard must keep the body from running even once */
static int __attribute__((noinline)) sum_zero(int n)
{
  int s = 100;
  for (int i = 0; i < n; i++)
    s += gv[i];
  return s;
}

int main(void)
{
  fill();

  if (sum_g8() != 8 * 0xaa)
    abort();
  if (sum_gn(0) != 0)
    abort();
  if (sum_gn(1) != 0xaa)
    abort();
  if (sum_gn(88) != 88 * 0xaa)
    abort();
  if (sum_gw(24) != 3 * (23 * 24 / 2))
    abort();
  if (sum_zero(0) != 100)
    abort();
  if (sum_zero(-5) != 100)
    abort();

  set_range(0, 5, 0x55);
  if (check_ranges(5, 88) != 88)
    abort();
  if (count_and_flag(5, 0x55) != 5)
    abort();
  if (sink != 0)
    abort(); /* count_and_flag(5, 0x55) must never take the else branch after fill+set_range */
  if (count_and_flag(88, 0x55) != 5)
    abort();
  if (sink != 87)
    abort();

  printf("sum_g8=%d check=%d\n", sum_g8(), check_ranges(5, 88));
  printf("OK\n");
  return 0;
}
