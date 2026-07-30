/* 64-bit values must NOT be if-converted into a SELECT: the backend's
 * select lowering (tcc_gen_machine_select_mop) moves a single core register
 * inside an ITE block, so an INT64 select silently keeps whatever the high
 * register held.  Found via self-hosting: the cross compiled known_bits.c's
 *   uint64_t mask = (width == 64) ? ~0ULL : 0xFFFFFFFFULL;
 * into an ITE writing only the low word — the high word kept an unrelated
 * live value (dest_btype), so on-device known_bits folded -2<<32 to 0 and
 * every 64-bit constant fold downstream collapsed (test_llong_mul_64bit
 * printed 0x1).  Each shape below hits one SELECT formation site:
 *
 *  1. mask_of      — simple ASSIGN diamond, equal LOW words so the vacuous
 *                    single-reg ITE looks plausible; highs differ.
 *  2. ret_sel      — RETURNVALUE diamond (new INT32 select vreg would
 *                    truncate the i64 constants).
 *  3. neg_flag     — SETIF + negate -> SELECT(#-1,#0) with an i64 dest.
 *  4. call_sel     — PARAM0/CALL diamond with i64 constant args.
 *  5. acc_loop     — symbolic-limit accumulator loop with an i64 IV
 *                    (loop_eliminate's i32 MUL/SELECT closed form and every
 *                    other IV consumer must skip non-INT32 IVs).
 *  6. pick_f/pick_d— FP const ternaries (FP select would predicate core-reg
 *                    moves on VFP values).
 */
#include <stdio.h>

volatile int w64 = 64, w32 = 32, one = 1, zero = 0;
volatile int n7 = 7, n0 = 0;

static unsigned long long mask_of(int width)
{
  unsigned long long mask = (width == 64) ? ~0ULL : 0xFFFFFFFFULL;
  return mask;
}

static long long ret_sel(int c)
{
  return c ? -1LL : 0xFFFFFFFFLL;
}

static long long neg_flag(int a, int b)
{
  return -(long long)(a < b);
}

static unsigned long long g_arg;
static void take(unsigned long long v) { g_arg = v; }

static unsigned long long call_sel(int c)
{
  if (c)
    take(0x1FFFFFFF0ULL);
  else
    take(16ULL);
  return g_arg;
}

static long long acc_loop(int n)
{
  long long acc = 0;
  for (int i = 0; i < n; i++)
    acc += 0x300000007LL;
  return acc;
}

static float pick_f(int c) { return c ? 1.5f : 2.5f; }
static double pick_d(int c) { return c ? 1.0e10 : -3.5; }

int main(void)
{
  printf("m=%llx %llx\n", mask_of(w64), mask_of(w32));
  printf("r=%llx %llx\n", (unsigned long long)ret_sel(zero),
         (unsigned long long)ret_sel(one));
  printf("n=%lld %lld\n", neg_flag(1, 2), neg_flag(2, 1));
  printf("c=%llx %llx\n", call_sel(one), call_sel(zero));
  printf("a=%llx %llx\n", (unsigned long long)acc_loop(n7),
         (unsigned long long)acc_loop(n0));
  printf("f=%.1f %.1f %.1f %.1f\n", (double)pick_f(one), (double)pick_f(zero),
         pick_d(zero), pick_d(one));

  /* The self-host scenario itself: a 64-bit shift whose operand came through
   * a masked select — wrong mask high words collapse this to 0. */
  unsigned long long v = (unsigned long long)(long long)-2;
  unsigned long long m = mask_of(w64);
  printf("s=%llx\n", (v & m) << 32);
  return 0;
}
