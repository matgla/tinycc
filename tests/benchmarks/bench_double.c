/*
 * Double-precision benchmarks -- the workload that tells you whether the
 * RP2350 DCP is worth anything.
 *
 * The rest of the suite has no `double` in it at all (bench_math.c's
 * "float_math" is an integer loop returning a constant), so before this file
 * a softfp-vs-rp2350fp comparison measured nothing and reported "no change".
 *
 * Each kernel isolates one __aeabi_ entry point so the per-op win is visible
 * rather than averaged away:
 *
 *   double_add   dadd            6 DCP instructions vs a soft-float call
 *   double_mul   dmul           13 DCP instructions, 7 scratch registers
 *   double_div   ddiv           CONTROL: soft in both builds, expect ~0% change
 *   double_cmp   cdcmple        the structural win -- flags, not a call
 *   double_mixed add/sub/mul/cmp a realistic kernel in realistic proportions
 *
 * `double_div` earns its place by being the null result: if it moves, the
 * measurement is picking up something other than the DCP (clock setup, link
 * layout, iteration-count drift) and the other numbers can't be trusted.
 *
 * Rules every kernel here follows:
 *
 *   - Stay in the normal range.  The DCP flushes subnormals to zero and
 *     libsoftfp does not, so a subnormal anywhere would make the two builds
 *     disagree and turn the shared expected-result check into a false alarm.
 *   - Return a deterministic int.  Both builds are correctly rounded over the
 *     normal range (proven by tests/fp: 2898/3057, every failure subnormal),
 *     so they must produce bit-identical results -- which makes
 *     register_benchmark_ex's expected value a free correctness cross-check
 *     on the DCP sequences at speed, not just a timing harness.
 *   - Keep the accumulator live via the return value so the loop cannot be
 *     optimised away.
 */

#include "benchmarks.h"

void init_double_benchmarks(void);

/* ---------------------------------------------------------------------------
 * dadd: alternating add/sub around a running total.
 *
 * Terms are chosen to keep the accumulator O(1) and the exponents close, so
 * every iteration is a real significand add rather than a degenerate
 * "add zero to a huge number" that the DCP and soft float both shortcut.
 * ------------------------------------------------------------------------- */
int bench_double_add(int iterations)
{
  double acc = 1.0;
  int n;

  for (n = 0; n < iterations; n++)
  {
    double t = (double)(n & 0x3F) * 0.015625 + 0.5; /* [0.5, 1.484], exact */
    acc = acc + t;
    acc = acc - (t * 0.5);
    acc = acc - (t * 0.5);
    acc = acc + 0.0009765625; /* 2^-10, keeps acc drifting upward */
  }

  return (int)(acc * 1024.0);
}

/* ---------------------------------------------------------------------------
 * dmul: multiply chain, renormalised so it neither overflows nor decays into
 * subnormals.  The x2 factor pulls back toward 1.0 whenever the running value
 * strays, which keeps every multiply in the normal range.
 * ------------------------------------------------------------------------- */
int bench_double_mul(int iterations)
{
  double acc = 1.0;
  int n;

  for (n = 0; n < iterations; n++)
  {
    double x = 1.0 + (double)(n & 0x0F) * 0.0625; /* [1.0, 1.9375], exact */
    acc = acc * x;
    acc = acc * (1.0 / x); /* exact reciprocal only for powers of two -- the
                            * point is the multiply, not the identity */
    acc = acc * 1.0009765625;
    if (acc > 16.0)
      acc = acc * 0.0625;
  }

  return (int)(acc * 4096.0);
}

/* ---------------------------------------------------------------------------
 * ddiv: the control.  Division has no DCP sequence in librp2350fp -- it is
 * lib/fp/soft/ddiv.c in both builds -- so this should not move.
 * ------------------------------------------------------------------------- */
int bench_double_div(int iterations)
{
  double acc = 1.0;
  int n;

  for (n = 0; n < iterations; n++)
  {
    double d = 1.0 + (double)(n & 0x1F) * 0.03125; /* [1.0, 1.96875], exact */
    acc = acc / d;
    acc = acc / (1.0 / d);
    acc = acc / 0.999755859375;
    if (acc > 16.0)
      acc = acc * 0.0625;
  }

  return (int)(acc * 4096.0);
}

/* ---------------------------------------------------------------------------
 * cdcmple: comparison-dominated.  This is where the DCP should win by the most
 * structurally -- RCMP writes NZCV, so a compare is 4 instructions and a
 * branch, against a full call plus flag decode for soft float.
 *
 * Both orderings and both operand positions are exercised so the operand-swap
 * path in ir/gen/float.c (>, >= become reversed <, <=) is covered too.
 * ------------------------------------------------------------------------- */
int bench_double_cmp(int iterations)
{
  double a = 0.5;
  int hits = 0;
  int n;

  for (n = 0; n < iterations; n++)
  {
    double b = (double)(n & 0x7F) * 0.0078125; /* [0, 0.9921875], exact */

    if (a < b)
      hits += 1;
    if (a <= b)
      hits += 2;
    if (a > b)
      hits += 4;
    if (a >= b)
      hits += 8;
    if (a == b)
      hits += 16;

    /* Move `a` so the branch outcomes are not constant across iterations. */
    a = a + 0.00390625;
    if (a > 1.0)
      a = 0.5;
  }

  return hits;
}

/* ---------------------------------------------------------------------------
 * Mixed kernel: a bounded Mandelbrot-style iteration.  Two multiplies, three
 * adds, one compare per step, which is roughly the shape real double code has,
 * and the escape test makes the compare feed a branch rather than a value.
 * ------------------------------------------------------------------------- */
int bench_double_mixed(int iterations)
{
  int total = 0;
  int n;

  for (n = 0; n < iterations; n++)
  {
    double cr = -2.0 + (double)(n & 0x1F) * 0.09375; /* [-2.0, 0.90625] */
    double ci = -1.25 + (double)((n >> 5) & 0x0F) * 0.15625;
    double zr = 0.0, zi = 0.0;
    int k;

    for (k = 0; k < 16; k++)
    {
      double zr2 = zr * zr;
      double zi2 = zi * zi;
      if (zr2 + zi2 > 4.0)
        break;
      zi = 2.0 * zr * zi + ci;
      zr = zr2 - zi2 + cr;
    }
    total += k;
  }

  return total;
}

void init_double_benchmarks(void)
{
  /* Expected values were measured on the libsoftfp build, which tests/fp
   * proves bit-exact.  They then act as a correctness cross-check on every
   * other FP configuration: softfp and the DCP are both correctly rounded over
   * the normal range, so these must match exactly.  A FAIL in the Verify
   * column is a real miscompare in the DCP sequences, not timing noise. */
  register_benchmark_ex("double_add", bench_double_add, 1000, "Double add/sub chain (dadd)", 2024);
  register_benchmark_ex("double_mul", bench_double_mul, 1000, "Double multiply chain (dmul)", 10870);
  register_benchmark_ex("double_div", bench_double_div, 1000, "Double divide chain (ddiv, soft in both)", 5228);
  register_benchmark_ex("double_cmp", bench_double_cmp, 1000, "Double comparisons (cdcmple)", 11949);
  register_benchmark_ex("double_mixed", bench_double_mixed, 200, "Mandelbrot kernel (mul+add+cmp)", 960);
}
