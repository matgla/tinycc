/*
 * host_softfp_test.c - run the FP conformance vectors directly against the
 * lib/fp/soft sources, natively on the build host.
 *
 * Why this exists in addition to tests/ir_tests/421_fp_conformance.c:
 *
 *   - The soft-float library is pure integer C, so it can be compiled and
 *     exercised on x86 with no target, QEMU or hardware in the loop.  That
 *     turns a multi-minute edit/flash/observe cycle into a sub-second one.
 *   - It calls the `__aeabi_*` entry points *by name*.  The target-side test
 *     uses C operators, so what it exercises depends on which library the link
 *     actually resolved -- and that has silently been libgcc rather than
 *     libsoftfp (lib/fp/libsoftfp.so shadows libsoftfp.a for `-lsoftfp`).
 *     Calling the symbols directly removes that ambiguity entirely.
 *
 * Build and run:
 *     tests/fp/run_host_softfp_test.sh
 *
 * Exit status is the number of failing vectors (0 == pass).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fp_conformance.h"
#include "fp_vectors.h"

/* ───── the library under test ───── */

extern double __aeabi_dadd(double, double);
extern double __aeabi_dsub(double, double);
extern double __aeabi_dmul(double, double);
extern double __aeabi_ddiv(double, double);
extern double __aeabi_dneg(double);
extern int __aeabi_dcmplt(double, double);
extern int __aeabi_dcmple(double, double);
extern int __aeabi_dcmpeq(double, double);
extern int __aeabi_dcmpge(double, double);
extern int __aeabi_dcmpgt(double, double);
extern double __aeabi_i2d(int);
extern double __aeabi_ui2d(unsigned);
extern int __aeabi_d2iz(double);
extern unsigned __aeabi_d2uiz(double);
extern long long __aeabi_d2lz(double);
extern unsigned long long __aeabi_d2ulz(double);
extern double __aeabi_f2d_bits(uint32_t);
extern float __aeabi_d2f(double);

extern float __aeabi_fadd(float, float);
extern float __aeabi_fsub(float, float);
extern float __aeabi_fmul(float, float);
extern float __aeabi_fdiv(float, float);
extern int __aeabi_fcmplt(float, float);
extern int __aeabi_fcmple(float, float);
extern int __aeabi_fcmpeq(float, float);
extern int __aeabi_fcmpge(float, float);
extern int __aeabi_fcmpgt(float, float);
extern float __aeabi_i2f(int);
extern float __aeabi_ui2f(unsigned);
extern int __aeabi_f2iz(float);
extern unsigned __aeabi_f2uiz(float);

/* dconv.c calls these explicitly rather than using C shifts (the target
 * codegen for 64-bit shifts has been unreliable); supply them for the host. */
unsigned long long __aeabi_llsr(unsigned long long a, int b) { return b >= 64 ? 0ull : a >> b; }
long long __aeabi_llsl(long long a, int b) { return b >= 64 ? 0ll : (long long)((unsigned long long)a << b); }

/* ───── bit punning ───── */

static double d_from_bits(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static uint64_t d_to_bits(double d) { uint64_t b; memcpy(&b, &d, 8); return b; }
static float f_from_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t f_to_bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

static int d_is_nan_bits(uint64_t b)
{
  return ((b & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) && ((b & 0x000fffffffffffffull) != 0);
}
static int f_is_nan_bits(uint32_t b)
{
  return ((b & 0x7f800000u) == 0x7f800000u) && ((b & 0x007fffffu) != 0);
}

static const char *op_name(unsigned op)
{
  switch (op)
  {
  case FPOP_ADD: return "add";     case FPOP_SUB: return "sub";
  case FPOP_MUL: return "mul";     case FPOP_DIV: return "div";
  case FPOP_CMPALL: return "cmp";  case FPOP_NEG: return "neg";
  case FPOP_I2F: return "i2f";     case FPOP_U2F: return "u2f";
  case FPOP_L2F: return "l2f";     case FPOP_UL2F: return "ul2f";
  case FPOP_F2I: return "f2i";     case FPOP_F2U: return "f2u";
  case FPOP_F2L: return "f2l";     case FPOP_F2UL: return "f2ul";
  case FPOP_WIDEN: return "widen"; case FPOP_NARROW: return "narrow";
  default: return "?";
  }
}

static int max_report = 20;
static int reported;

/* Per-op tallies make it obvious whether a fix landed everywhere or only in
 * the one routine that was edited. */
#define NOPS 24
static int fail_by_op[2][NOPS];
static int run_by_op[2][NOPS];

int main(int argc, char **argv)
{
  int failures = 0, i;

  if (argc > 1)
    max_report = atoi(argv[1]);

  for (i = 0; i < FP_VECTORS_D_COUNT; i++)
  {
    const fp_vec64 *v = &fp_vectors_d[i];
    double a = d_from_bits(v->a), b = d_from_bits(v->b);
    uint64_t got;
    int ok;

    run_by_op[0][v->op]++;

    switch (v->op)
    {
    case FPOP_ADD: got = d_to_bits(__aeabi_dadd(a, b)); break;
    case FPOP_SUB: got = d_to_bits(__aeabi_dsub(a, b)); break;
    case FPOP_MUL: got = d_to_bits(__aeabi_dmul(a, b)); break;
    case FPOP_DIV: got = d_to_bits(__aeabi_ddiv(a, b)); break;
    case FPOP_NEG: got = d_to_bits(__aeabi_dneg(a)); break;
    case FPOP_CMPALL:
      got = (uint64_t)((__aeabi_dcmplt(a, b) ? FPC_LT : 0u) | (__aeabi_dcmple(a, b) ? FPC_LE : 0u) |
                       (__aeabi_dcmpeq(a, b) ? FPC_EQ : 0u) | (!__aeabi_dcmpeq(a, b) ? FPC_NE : 0u) |
                       (__aeabi_dcmpge(a, b) ? FPC_GE : 0u) | (__aeabi_dcmpgt(a, b) ? FPC_GT : 0u));
      break;
    case FPOP_I2F: got = d_to_bits(__aeabi_i2d((int)(uint32_t)v->a)); break;
    case FPOP_U2F: got = d_to_bits(__aeabi_ui2d((unsigned)v->a)); break;
    case FPOP_F2I: got = (uint64_t)(uint32_t)__aeabi_d2iz(a); break;
    case FPOP_F2U: got = (uint64_t)__aeabi_d2uiz(a); break;
    case FPOP_F2L: got = (uint64_t)__aeabi_d2lz(a); break;
    case FPOP_F2UL: got = (uint64_t)__aeabi_d2ulz(a); break;
    case FPOP_WIDEN: got = d_to_bits(__aeabi_f2d_bits((uint32_t)v->a)); break;
    case FPOP_NARROW: got = (uint64_t)f_to_bits(__aeabi_d2f(a)); break;
    /* l2d / ul2d are not implemented by libsoftfp at all -- see the report in
     * the runner script; skip rather than silently link something else. */
    case FPOP_L2F:
    case FPOP_UL2F:
    default: run_by_op[0][v->op]--; continue;
    }

    ok = (v->flags & FPF_NAN_RESULT)
             ? ((v->op == FPOP_NARROW) ? f_is_nan_bits((uint32_t)got) : d_is_nan_bits(got))
             : (got == v->r);
    if (!ok)
    {
      failures++;
      fail_by_op[0][v->op]++;
      if (reported++ < max_report)
        printf("FAIL d[%d] %-6s a=%016llx b=%016llx want=%016llx got=%016llx\n", i, op_name(v->op),
               (unsigned long long)v->a, (unsigned long long)v->b, (unsigned long long)v->r,
               (unsigned long long)got);
    }
  }

  for (i = 0; i < FP_VECTORS_F_COUNT; i++)
  {
    const fp_vec32 *v = &fp_vectors_f[i];
    float a = f_from_bits(v->a), b = f_from_bits(v->b);
    uint32_t got;
    int ok;

    run_by_op[1][v->op]++;

    switch (v->op)
    {
    case FPOP_ADD: got = f_to_bits(__aeabi_fadd(a, b)); break;
    case FPOP_SUB: got = f_to_bits(__aeabi_fsub(a, b)); break;
    case FPOP_MUL: got = f_to_bits(__aeabi_fmul(a, b)); break;
    case FPOP_DIV: got = f_to_bits(__aeabi_fdiv(a, b)); break;
    case FPOP_NEG: got = f_to_bits(a) ^ 0x80000000u; break; /* no __aeabi_fneg in libsoftfp */
    case FPOP_CMPALL:
      got = (uint32_t)((__aeabi_fcmplt(a, b) ? FPC_LT : 0u) | (__aeabi_fcmple(a, b) ? FPC_LE : 0u) |
                       (__aeabi_fcmpeq(a, b) ? FPC_EQ : 0u) | (!__aeabi_fcmpeq(a, b) ? FPC_NE : 0u) |
                       (__aeabi_fcmpge(a, b) ? FPC_GE : 0u) | (__aeabi_fcmpgt(a, b) ? FPC_GT : 0u));
      break;
    case FPOP_I2F: got = f_to_bits(__aeabi_i2f((int)v->a)); break;
    case FPOP_U2F: got = f_to_bits(__aeabi_ui2f(v->a)); break;
    case FPOP_F2I: got = (uint32_t)__aeabi_f2iz(a); break;
    case FPOP_F2U: got = __aeabi_f2uiz(a); break;
    default: run_by_op[1][v->op]--; continue;
    }

    ok = (v->flags & FPF_NAN_RESULT) ? f_is_nan_bits(got) : (got == v->r);
    if (!ok)
    {
      failures++;
      fail_by_op[1][v->op]++;
      if (reported++ < max_report)
        printf("FAIL f[%d] %-6s a=%08x b=%08x want=%08x got=%08x\n", i, op_name(v->op), v->a, v->b, v->r, got);
    }
  }

  if (reported > max_report)
    printf("... %d further failures suppressed\n", reported - max_report);

  printf("\n%-8s %8s %8s %8s\n", "op", "run", "failed", "");
  for (int p = 0; p < 2; p++)
    for (i = 0; i < NOPS; i++)
      if (run_by_op[p][i])
        printf("%c %-6s %8d %8d %s\n", p ? 'f' : 'd', op_name(i), run_by_op[p][i], fail_by_op[p][i],
               fail_by_op[p][i] ? "  <-- FAIL" : "");

  printf("\nlibsoftfp host conformance: %d failures\n", failures);
  return failures != 0;
}
