/*
 * fp_conformance.c - walks the generated vector tables on the target and
 * checks every result bit-exactly against the host-computed reference.
 *
 * This file is deliberately plain C with no target dependencies beyond
 * printf, so the same object can run under QEMU (mps2-an505, semihosting) and
 * on real RP2350 hardware (Pico SDK stdio over UART).
 *
 * Everything here goes through the compiler's normal FP lowering, so what is
 * actually under test is whatever `-mfpu=` selected: `__aeabi_*` calls into
 * libsoftfp / librp2350fp, or inline DCP / VFP sequences.
 */

#include <stdio.h>
#include <string.h>

#include "fp_conformance.h"
#include "fp_vectors.h"

/* Cap the noise when something is badly broken: a wrong __aeabi_dadd would
 * otherwise emit thousands of lines over a 460800-baud UART. */
#define FP_MAX_REPORTED_FAILURES 20

static int reported;

/* Per-op tallies, printed as a histogram at the end.  The 20-line cap above
 * keeps the UART usable but hides the shape of a failure -- and the shape is
 * what identifies the cause.  When librp2350fp first ran on hardware the
 * visible 20 lines were all `add`, which read as "dadd is broken"; the
 * histogram showed add/sub/mul/widen/narrow failing and div/conversions clean,
 * i.e. exactly the DCP-implemented ops and not the soft ones -- a completely
 * different diagnosis.  A "subnormal operand or result" column is carried
 * alongside because that is the axis these failures actually separate on. */
#define FP_OP_COUNT (FPOP_NARROW + 1)

/* bank 0 = double vectors, bank 1 = float vectors */
#define FP_BANK_D 0
#define FP_BANK_F 1

static int fail_by_op[2][FP_OP_COUNT];
static int fail_by_op_subnormal[2][FP_OP_COUNT];
static int total_by_op[2][FP_OP_COUNT];

/* Vectors whose divergence is the documented flush-to-zero deviation rather
 * than a defect (see below).  Reported only alongside real failures: a clean
 * run's output has to stay the three lines 421_fp_conformance.expect matches,
 * and that one file has to serve both the soft-float and the DCP build. */
static int tolerated_ftz;

/* The RP2350 double coprocessor flushes subnormal operands and results to zero
 * and offers no path that doesn't -- pico-sdk's own double_aeabi_dcp.S behaves
 * identically and says so.  docs/userspace_floating_point.md documents this as
 * the accepted trade-off of CONFIG_BUILD_USERSPACE_HARDWARE_FP on that part.
 *
 * So on a DCP build a double-bank mismatch with a subnormal operand or
 * reference result is expected behaviour, and counting it as a failure would
 * leave this gate permanently red -- which costs more than it catches, because
 * a red gate stops reporting the divergences that ARE defects.  Those still
 * fail: the tolerance is exactly "one of the values involved is subnormal",
 * nothing wider, and single precision (FPv5-SP, fully IEEE) is never
 * tolerated.  tcc defines the macro only for -mfpu=rp2350.
 *
 * The benchmark runner reaches the same conclusion from the outside with
 * `tests/benchmarks/run_fp_conformance.py --allow-ftz`. */
#ifdef __TCC_DOUBLE_FLUSHES_SUBNORMALS__
#define FP_TOLERATE_SUBNORMAL_DOUBLE 1
#else
#define FP_TOLERATE_SUBNORMAL_DOUBLE 0
#endif

static int d_is_subnormal_bits(uint64_t b)
{
  return ((b >> 52) & 0x7FFu) == 0 && (b & 0x000FFFFFFFFFFFFFull) != 0;
}

static int f_is_subnormal_bits(uint32_t b)
{
  return ((b >> 23) & 0xFFu) == 0 && (b & 0x007FFFFFu) != 0;
}

static void tally(int bank, int op, int failed, int subnormal)
{
  if (op < 0 || op >= FP_OP_COUNT)
    return;
  total_by_op[bank][op]++;
  if (!failed)
    return;
  fail_by_op[bank][op]++;
  if (subnormal)
    fail_by_op_subnormal[bank][op]++;
}

/* ───── bit punning ─────
 *
 * memcpy rather than a union: the soft-float sources in lib/fp carry a comment
 * (soft_common.h) that address-taken 64-bit union locals have been miscompiled
 * on this target, and a conformance test must not depend on the thing it is
 * trying to validate.
 */

static double d_from_bits(uint64_t b)
{
  double d;
  memcpy(&d, &b, 8);
  return d;
}

static uint64_t d_to_bits(double d)
{
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

static float f_from_bits(uint32_t b)
{
  float f;
  memcpy(&f, &b, 4);
  return f;
}

static uint32_t f_to_bits(float f)
{
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}

static int d_is_nan_bits(uint64_t b)
{
  return ((b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) && ((b & 0x000fffffffffffffull) != 0);
}

static int f_is_nan_bits(uint32_t b)
{
  return ((b & 0x7f800000u) == 0x7f800000u) && ((b & 0x007fffffu) != 0);
}

/* printf on a 32-bit target has no portable 64-bit hex specifier we can rely
 * on across newlib and the Pico SDK, so split into two words. */
static void print_u64(uint64_t v)
{
  printf("%08x%08x", (unsigned)(uint32_t)(v >> 32), (unsigned)(uint32_t)v);
}

static const char *op_name(unsigned op)
{
  switch (op)
  {
  case FPOP_ADD:    return "add";
  case FPOP_SUB:    return "sub";
  case FPOP_MUL:    return "mul";
  case FPOP_DIV:    return "div";
  case FPOP_CMPALL: return "cmp";
  case FPOP_NEG:    return "neg";
  case FPOP_I2F:    return "i2f";
  case FPOP_U2F:    return "u2f";
  case FPOP_L2F:    return "l2f";
  case FPOP_UL2F:   return "ul2f";
  case FPOP_F2I:    return "f2i";
  case FPOP_F2U:    return "f2u";
  case FPOP_F2L:    return "f2l";
  case FPOP_F2UL:   return "f2ul";
  case FPOP_WIDEN:  return "widen";
  case FPOP_NARROW: return "narrow";
  default:          return "?";
  }
}

static void fail_d(int idx, const fp_vec64 *v, uint64_t got)
{
  if (reported++ >= FP_MAX_REPORTED_FAILURES)
    return;
  printf("FP FAIL d[%d] %s a=", idx, op_name(v->op));
  print_u64(v->a);
  printf(" b=");
  print_u64(v->b);
  printf(" want=");
  print_u64(v->r);
  printf(" got=");
  print_u64(got);
  printf("\r\n");
}

static void fail_f(int idx, const fp_vec32 *v, uint32_t got)
{
  if (reported++ >= FP_MAX_REPORTED_FAILURES)
    return;
  printf("FP FAIL f[%d] %s a=%08x b=%08x want=%08x got=%08x\r\n", idx, op_name(v->op),
         (unsigned)v->a, (unsigned)v->b, (unsigned)v->r, (unsigned)got);
}

int fp_conformance_run_double(void)
{
  int failures = 0;
  int i;

  for (i = 0; i < FP_VECTORS_D_COUNT; i++)
  {
    const fp_vec64 *v = &fp_vectors_d[i];
    double a = d_from_bits(v->a);
    double b = d_from_bits(v->b);
    uint64_t got;
    int sub;

    switch (v->op)
    {
    case FPOP_ADD: got = d_to_bits(a + b); break;
    case FPOP_SUB: got = d_to_bits(a - b); break;
    case FPOP_MUL: got = d_to_bits(a * b); break;
    case FPOP_DIV: got = d_to_bits(a / b); break;
    case FPOP_NEG: got = d_to_bits(-a); break;

    case FPOP_CMPALL:
      got = (uint64_t)((a < b ? FPC_LT : 0u) | (a <= b ? FPC_LE : 0u) | (a == b ? FPC_EQ : 0u) |
                       (a != b ? FPC_NE : 0u) | (a >= b ? FPC_GE : 0u) | (a > b ? FPC_GT : 0u));
      break;

    case FPOP_I2F:  got = d_to_bits((double)(int32_t)(uint32_t)v->a); break;
    case FPOP_U2F:  got = d_to_bits((double)(uint32_t)v->a); break;
    case FPOP_L2F:  got = d_to_bits((double)(int64_t)v->a); break;
    case FPOP_UL2F: got = d_to_bits((double)v->a); break;

    case FPOP_F2I:  got = (uint64_t)(uint32_t)(int32_t)a; break;
    case FPOP_F2U:  got = (uint64_t)(uint32_t)a; break;
    case FPOP_F2L:  got = (uint64_t)(int64_t)a; break;
    case FPOP_F2UL: got = (uint64_t)a; break;

    case FPOP_WIDEN:  got = d_to_bits((double)f_from_bits((uint32_t)v->a)); break;
    case FPOP_NARROW: got = (uint64_t)f_to_bits((float)a); break;

    default: continue;
    }

    /* WIDEN's operand is a float in the low word; every other op's operands
     * are doubles.  NARROW's result is a float. */
    sub = (v->op == FPOP_WIDEN)
              ? f_is_subnormal_bits((uint32_t)v->a)
              : (d_is_subnormal_bits(v->a) || d_is_subnormal_bits(v->b));
    sub = sub || ((v->op == FPOP_NARROW) ? f_is_subnormal_bits((uint32_t)v->r)
                                         : d_is_subnormal_bits(v->r));

    if (v->flags & FPF_NAN_RESULT)
    {
      /* the payload is unspecified; only NaN-ness is required */
      int ok = (v->op == FPOP_NARROW) ? f_is_nan_bits((uint32_t)got) : d_is_nan_bits(got);
      int bad = !ok;
      if (bad && sub && FP_TOLERATE_SUBNORMAL_DOUBLE)
      {
        tolerated_ftz++;
        bad = 0;
      }
      tally(FP_BANK_D, v->op, bad, sub);
      if (bad)
      {
        failures++;
        fail_d(i, v, got);
      }
      continue;
    }

    {
      int bad = (got != v->r);
      if (bad && sub && FP_TOLERATE_SUBNORMAL_DOUBLE)
      {
        tolerated_ftz++;
        bad = 0;
      }
      tally(FP_BANK_D, v->op, bad, sub);
      if (bad)
      {
        failures++;
        fail_d(i, v, got);
      }
    }
  }

  return failures;
}

int fp_conformance_run_float(void)
{
  int failures = 0;
  int i;

  for (i = 0; i < FP_VECTORS_F_COUNT; i++)
  {
    const fp_vec32 *v = &fp_vectors_f[i];
    float a = f_from_bits(v->a);
    float b = f_from_bits(v->b);
    uint32_t got;
    int sub;

    switch (v->op)
    {
    case FPOP_ADD: got = f_to_bits(a + b); break;
    case FPOP_SUB: got = f_to_bits(a - b); break;
    case FPOP_MUL: got = f_to_bits(a * b); break;
    case FPOP_DIV: got = f_to_bits(a / b); break;
    case FPOP_NEG: got = f_to_bits(-a); break;

    case FPOP_CMPALL:
      got = (uint32_t)((a < b ? FPC_LT : 0u) | (a <= b ? FPC_LE : 0u) | (a == b ? FPC_EQ : 0u) |
                       (a != b ? FPC_NE : 0u) | (a >= b ? FPC_GE : 0u) | (a > b ? FPC_GT : 0u));
      break;

    case FPOP_I2F: got = f_to_bits((float)(int32_t)v->a); break;
    case FPOP_U2F: got = f_to_bits((float)v->a); break;

    case FPOP_F2I: got = (uint32_t)(int32_t)a; break;
    case FPOP_F2U: got = (uint32_t)a; break;

    default: continue;
    }

    sub = f_is_subnormal_bits(v->a) || f_is_subnormal_bits(v->b) || f_is_subnormal_bits(v->r);

    if (v->flags & FPF_NAN_RESULT)
    {
      int ok = f_is_nan_bits(got);
      tally(FP_BANK_F, v->op, !ok, sub);
      if (!ok)
      {
        failures++;
        fail_f(i, v, got);
      }
      continue;
    }

    tally(FP_BANK_F, v->op, got != v->r, sub);
    if (got != v->r)
    {
      failures++;
      fail_f(i, v, got);
    }
  }

  return failures;
}

/* One line per op that failed at all:
 *   FP BY-OP d add     159/ 918 failed (159 subnormal)
 * The subnormal column is what separates "this op is broken" from "this op
 * does not implement subnormals" -- if the two numbers are equal, every
 * failure had a subnormal operand or result and the op is otherwise exact. */
static void print_histogram(int bank, const char *tag)
{
  int op;
  for (op = 0; op < FP_OP_COUNT; op++)
  {
    if (fail_by_op[bank][op] == 0)
      continue;
    printf("FP BY-OP %s %-6s %4d/%4d failed (%d subnormal)\r\n", tag, op_name(op), fail_by_op[bank][op],
           total_by_op[bank][op], fail_by_op_subnormal[bank][op]);
  }
  for (op = 0; op < FP_OP_COUNT; op++)
  {
    if (total_by_op[bank][op] != 0 && fail_by_op[bank][op] == 0)
      printf("FP BY-OP %s %-6s %4d/%4d ok\r\n", tag, op_name(op), total_by_op[bank][op], total_by_op[bank][op]);
  }
}

int fp_conformance_run(void)
{
  int fd, ff;

  reported = 0;
  tolerated_ftz = 0;
  memset(fail_by_op, 0, sizeof(fail_by_op));
  memset(fail_by_op_subnormal, 0, sizeof(fail_by_op_subnormal));
  memset(total_by_op, 0, sizeof(total_by_op));

  printf("FP conformance: %d double vectors, %d float vectors\r\n", FP_VECTORS_D_COUNT, FP_VECTORS_F_COUNT);

  fd = fp_conformance_run_double();
  ff = fp_conformance_run_float();

  if (reported > FP_MAX_REPORTED_FAILURES)
    printf("FP ... %d further failures suppressed\r\n", reported - FP_MAX_REPORTED_FAILURES);

  /* Only on failure: a clean run's output stays exactly three lines, which is
   * what tests/ir_tests/421_fp_conformance.expect matches against. */
  if (fd + ff != 0)
  {
    int bank, op, sub = 0;
    print_histogram(FP_BANK_D, "d");
    print_histogram(FP_BANK_F, "f");
    for (bank = 0; bank < 2; bank++)
      for (op = 0; op < FP_OP_COUNT; op++)
        sub += fail_by_op_subnormal[bank][op];
    /* The RP2350 DCP has no subnormal support -- it flushes to zero, as does
     * pico-sdk's own double_aeabi_dcp.S.  Splitting the count lets the runner
     * accept that deviation (--allow-ftz) while still failing on anything
     * else, so the gate stays meaningful instead of permanently red. */
    printf("FP conformance: %d of %d failures involve subnormals\r\n", sub, fd + ff);
    if (tolerated_ftz)
      printf("FP conformance: %d subnormal double deviations tolerated (flush-to-zero build)\r\n", tolerated_ftz);
  }

  printf("FP conformance: double %d failed / %d, float %d failed / %d\r\n", fd, FP_VECTORS_D_COUNT, ff,
         FP_VECTORS_F_COUNT);

  return fd + ff;
}
