/*
 * gen_fp_vectors.c - host-side generator for the floating-point conformance
 * vector tables consumed by tests/fp/fp_conformance.c.
 *
 * Why generate on the host rather than diff TCC against GCC on target:
 *
 *   - x86-64 `double`/`float` are IEEE-754 binary64/binary32 evaluated in SSE
 *     with round-to-nearest-even, exactly matching what ARMv8-M must produce.
 *     So the host result *is* the reference, and a bug present in both TCC and
 *     GCC on target would still be caught.
 *   - One table serves every target FP implementation: libsoftfp today, the
 *     RP2350 DCP later, VFP after that. If the DCP disagrees with soft float
 *     by one ulp we want that to fail, and a TCC-vs-GCC diff would not show it.
 *   - Bit-exactness is specifically load-bearing on RP2350: the Pico SDK ships
 *     deliberately-not-correctly-rounded `ddiv_fast`/`sqrt_fast` next to the
 *     correctly-rounded `__aeabi_ddiv`, so "close enough" is not a pass.
 *
 * Build and run:
 *     cc -O2 -o gen_fp_vectors gen_fp_vectors.c -lm && ./gen_fp_vectors > fp_vectors.h
 *
 * The generated header is checked in so target builds never need this program.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ───── operation codes (kept in sync with fp_conformance.c) ───── */

enum
{
  FPOP_ADD = 0,
  FPOP_SUB,
  FPOP_MUL,
  FPOP_DIV,
  FPOP_CMPALL,
  FPOP_NEG,
  /* conversions: source is `a`, `b` unused */
  FPOP_I2F, /* int32  -> float/double */
  FPOP_U2F, /* uint32 -> float/double */
  FPOP_L2F, /* int64  -> float/double */
  FPOP_UL2F,/* uint64 -> float/double */
  FPOP_F2I, /* float/double -> int32  (truncate) */
  FPOP_F2U, /* float/double -> uint32 (truncate) */
  FPOP_F2L, /* float/double -> int64  (truncate) */
  FPOP_F2UL,/* float/double -> uint64 (truncate) */
  FPOP_WIDEN,  /* float -> double  (double table only) */
  FPOP_NARROW, /* double -> float  (double table only) */
};

/* result of an operation that produced a NaN: IEEE-754 does not pin the
 * payload, so compare "is a NaN" instead of the exact bit pattern */
#define FPF_NAN_RESULT 1u

static const char *op_names[] = {
    "FPOP_ADD",  "FPOP_SUB", "FPOP_MUL", "FPOP_DIV",  "FPOP_CMPALL", "FPOP_NEG",   "FPOP_I2F",
    "FPOP_U2F",  "FPOP_L2F", "FPOP_UL2F","FPOP_F2I",  "FPOP_F2U",    "FPOP_F2L",   "FPOP_F2UL",
    "FPOP_WIDEN","FPOP_NARROW",
};

/* bit positions inside an FPOP_CMPALL result */
#define FPC_LT 0x01u
#define FPC_LE 0x02u
#define FPC_EQ 0x04u
#define FPC_NE 0x08u
#define FPC_GE 0x10u
#define FPC_GT 0x20u

/* ───── bit punning ───── */

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

/* ───── interesting operand pools ───── */

/* Chosen to cover every class the IEEE-754 code paths branch on, plus the
 * boundaries where rounding decisions flip. */
static const uint64_t dpool[] = {
    0x0000000000000000ull, /* +0 */
    0x8000000000000000ull, /* -0 */
    0x0000000000000001ull, /* smallest denormal */
    0x8000000000000001ull, /* -smallest denormal */
    0x000fffffffffffffull, /* largest denormal */
    0x0010000000000000ull, /* smallest normal */
    0x8010000000000000ull, /* -smallest normal */
    0x3ff0000000000000ull, /* 1.0 */
    0xbff0000000000000ull, /* -1.0 */
    0x3ff0000000000001ull, /* 1.0 + 1ulp */
    0x3fe0000000000000ull, /* 0.5 */
    0x4000000000000000ull, /* 2.0 */
    0x4008000000000000ull, /* 3.0 */
    0x3fd5555555555555ull, /* 1/3 */
    0x400921fb54442d18ull, /* pi */
    0x4005bf0a8b145769ull, /* e */
    0x7fefffffffffffffull, /* DBL_MAX */
    0xffefffffffffffffull, /* -DBL_MAX */
    0x7ff0000000000000ull, /* +inf */
    0xfff0000000000000ull, /* -inf */
    0x7ff8000000000000ull, /* quiet NaN */
    0xfff8000000000000ull, /* -quiet NaN */
    0x4330000000000000ull, /* 2^52, the integer/fraction boundary */
    0x4330000000000001ull, /* 2^52 + 1ulp */
    0x432fffffffffffffull, /* just below 2^52 */
    0x41efffffffffffffull, /* just below 2^32 */
    0x4160000000000000ull, /* 2^23 */
    0x3cb0000000000000ull, /* 2^-52 (1 ulp of 1.0) */
    0x0008000000000000ull, /* mid denormal */
    0x7fe0000000000000ull, /* large, overflows when doubled */
    0x0020000000000000ull, /* small, underflows when halved repeatedly */
    0x3ff8000000000000ull, /* 1.5 — ties-to-even fodder */
};

static const uint32_t fpool[] = {
    0x00000000u, /* +0 */
    0x80000000u, /* -0 */
    0x00000001u, /* smallest denormal */
    0x80000001u, /* -smallest denormal */
    0x007fffffu, /* largest denormal */
    0x00800000u, /* smallest normal */
    0x80800000u, /* -smallest normal */
    0x3f800000u, /* 1.0 */
    0xbf800000u, /* -1.0 */
    0x3f800001u, /* 1.0 + 1ulp */
    0x3f000000u, /* 0.5 */
    0x40000000u, /* 2.0 */
    0x40400000u, /* 3.0 */
    0x3eaaaaabu, /* 1/3 */
    0x40490fdbu, /* pi */
    0x402df854u, /* e */
    0x7f7fffffu, /* FLT_MAX */
    0xff7fffffu, /* -FLT_MAX */
    0x7f800000u, /* +inf */
    0xff800000u, /* -inf */
    0x7fc00000u, /* quiet NaN */
    0xffc00000u, /* -quiet NaN */
    0x4b000000u, /* 2^23, the integer/fraction boundary */
    0x4b000001u, /* 2^23 + 1ulp */
    0x4affffffu, /* just below 2^23 */
    0x4f7fffffu, /* just below 2^32 */
    0x33800000u, /* 2^-23 (1 ulp of 1.0) */
    0x00400000u, /* mid denormal */
    0x7f000000u, /* large, overflows when doubled */
    0x3fc00000u, /* 1.5 — ties-to-even fodder */
};

/* xorshift64* — fixed seed so the generated header is reproducible */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rng_next(void)
{
  uint64_t x = rng_state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  rng_state = x;
  return x * 0x2545F4914F6CDD1Dull;
}

/* ───── emission ───── */

static int dcount = 0, fcount = 0;

static void emit_d(int op, uint64_t a, uint64_t b, uint64_t r, unsigned flags)
{
  printf("    {%s, %uu, 0x%016llxull, 0x%016llxull, 0x%016llxull},\n", op_names[op], flags,
         (unsigned long long)a, (unsigned long long)b, (unsigned long long)r);
  dcount++;
}

static void emit_f(int op, uint32_t a, uint32_t b, uint32_t r, unsigned flags)
{
  printf("    {%s, %uu, 0x%08xu, 0x%08xu, 0x%08xu},\n", op_names[op], flags, a, b, r);
  fcount++;
}

/* Apply a binary/compare op on doubles and emit the vector. */
static void do_d_op(int op, uint64_t ab, uint64_t bb)
{
  double a = d_from_bits(ab), b = d_from_bits(bb);
  uint64_t r = 0;
  unsigned flags = 0;

  switch (op)
  {
  case FPOP_ADD: r = d_to_bits(a + b); break;
  case FPOP_SUB: r = d_to_bits(a - b); break;
  case FPOP_MUL: r = d_to_bits(a * b); break;
  case FPOP_DIV: r = d_to_bits(a / b); break;
  case FPOP_CMPALL:
    r = (uint64_t)((a < b ? FPC_LT : 0u) | (a <= b ? FPC_LE : 0u) | (a == b ? FPC_EQ : 0u) |
                   (a != b ? FPC_NE : 0u) | (a >= b ? FPC_GE : 0u) | (a > b ? FPC_GT : 0u));
    break;
  case FPOP_NEG: r = d_to_bits(-a); break;
  default: return;
  }

  if (op <= FPOP_DIV || op == FPOP_NEG)
  {
    if (isnan(d_from_bits(r)))
      flags |= FPF_NAN_RESULT;
  }
  emit_d(op, ab, bb, r, flags);
}

static void do_f_op(int op, uint32_t ab, uint32_t bb)
{
  float a = f_from_bits(ab), b = f_from_bits(bb);
  uint32_t r = 0;
  unsigned flags = 0;

  switch (op)
  {
  case FPOP_ADD: r = f_to_bits(a + b); break;
  case FPOP_SUB: r = f_to_bits(a - b); break;
  case FPOP_MUL: r = f_to_bits(a * b); break;
  case FPOP_DIV: r = f_to_bits(a / b); break;
  case FPOP_CMPALL:
    r = (uint32_t)((a < b ? FPC_LT : 0u) | (a <= b ? FPC_LE : 0u) | (a == b ? FPC_EQ : 0u) |
                   (a != b ? FPC_NE : 0u) | (a >= b ? FPC_GE : 0u) | (a > b ? FPC_GT : 0u));
    break;
  case FPOP_NEG: r = f_to_bits(-a); break;
  default: return;
  }

  if (op <= FPOP_DIV || op == FPOP_NEG)
  {
    if (isnan(f_from_bits(r)))
      flags |= FPF_NAN_RESULT;
  }
  emit_f(op, ab, bb, r, flags);
}

/* Conversions.  Out-of-range float->integer is undefined in C and
 * unspecified in the AEABI, so only in-range sources are emitted; the
 * saturation behaviour is deliberately not pinned down here. */

static int d_fits_i32(double d)  { return d >= -2147483648.0 && d < 2147483648.0; }
static int d_fits_u32(double d)  { return d >= 0.0 && d < 4294967296.0; }
static int d_fits_i64(double d)  { return d >= -9223372036854775808.0 && d < 9223372036854775808.0; }
static int d_fits_u64(double d)  { return d >= 0.0 && d < 18446744073709551616.0; }

static void emit_d_conversions(void)
{
  static const int32_t i32v[] = {0, 1, -1, 2, -2, 127, -128, 32767, -32768, 65535, 123456789,
                                 -123456789, 2147483647, -2147483647 - 1, 1 << 23, (1 << 23) + 1};
  static const uint32_t u32v[] = {0u, 1u, 2u, 255u, 65535u, 123456789u, 2147483648u, 4294967295u,
                                  16777216u, 16777217u};
  static const int64_t i64v[] = {0, 1, -1, 1000000007LL, -1000000007LL, (1LL << 52), (1LL << 52) + 1,
                                 (1LL << 53), (1LL << 53) + 1, 9223372036854775807LL,
                                 -9223372036854775807LL - 1};
  static const uint64_t u64v[] = {0ull, 1ull, (1ull << 52), (1ull << 53), (1ull << 53) + 1,
                                  (1ull << 63), 18446744073709551615ull};
  size_t i;

  for (i = 0; i < sizeof(i32v) / sizeof(i32v[0]); i++)
    emit_d(FPOP_I2F, (uint64_t)(uint32_t)i32v[i], 0, d_to_bits((double)i32v[i]), 0);
  for (i = 0; i < sizeof(u32v) / sizeof(u32v[0]); i++)
    emit_d(FPOP_U2F, (uint64_t)u32v[i], 0, d_to_bits((double)u32v[i]), 0);
  for (i = 0; i < sizeof(i64v) / sizeof(i64v[0]); i++)
    emit_d(FPOP_L2F, (uint64_t)i64v[i], 0, d_to_bits((double)i64v[i]), 0);
  for (i = 0; i < sizeof(u64v) / sizeof(u64v[0]); i++)
    emit_d(FPOP_UL2F, u64v[i], 0, d_to_bits((double)u64v[i]), 0);

  for (i = 0; i < sizeof(dpool) / sizeof(dpool[0]); i++)
  {
    double d = d_from_bits(dpool[i]);
    if (d_fits_i32(d))
      emit_d(FPOP_F2I, dpool[i], 0, (uint64_t)(uint32_t)(int32_t)d, 0);
    if (d_fits_u32(d))
      emit_d(FPOP_F2U, dpool[i], 0, (uint64_t)(uint32_t)d, 0);
    if (d_fits_i64(d))
      emit_d(FPOP_F2L, dpool[i], 0, (uint64_t)(int64_t)d, 0);
    if (d_fits_u64(d))
      emit_d(FPOP_F2UL, dpool[i], 0, (uint64_t)d, 0);
  }

  /* float -> double is exact; double -> float rounds, which is where the DCP
   * NRDF/RDFG path has to agree with everyone else. */
  for (i = 0; i < sizeof(fpool) / sizeof(fpool[0]); i++)
  {
    float f = f_from_bits(fpool[i]);
    uint64_t r = d_to_bits((double)f);
    emit_d(FPOP_WIDEN, (uint64_t)fpool[i], 0, r, isnan((double)f) ? FPF_NAN_RESULT : 0);
  }
  for (i = 0; i < sizeof(dpool) / sizeof(dpool[0]); i++)
  {
    double d = d_from_bits(dpool[i]);
    uint32_t r = f_to_bits((float)d);
    emit_d(FPOP_NARROW, dpool[i], 0, (uint64_t)r, isnan((float)d) ? FPF_NAN_RESULT : 0);
  }
}

static void emit_f_conversions(void)
{
  static const int32_t i32v[] = {0, 1, -1, 127, -128, 32767, -32768, 65535, 123456789,
                                 2147483647, -2147483647 - 1, 1 << 23, (1 << 23) + 1, (1 << 24) + 1};
  static const uint32_t u32v[] = {0u, 1u, 255u, 65535u, 123456789u, 2147483648u, 4294967295u,
                                  16777216u, 16777217u};
  size_t i;

  for (i = 0; i < sizeof(i32v) / sizeof(i32v[0]); i++)
    emit_f(FPOP_I2F, (uint32_t)i32v[i], 0, f_to_bits((float)i32v[i]), 0);
  for (i = 0; i < sizeof(u32v) / sizeof(u32v[0]); i++)
    emit_f(FPOP_U2F, u32v[i], 0, f_to_bits((float)u32v[i]), 0);

  for (i = 0; i < sizeof(fpool) / sizeof(fpool[0]); i++)
  {
    float f = f_from_bits(fpool[i]);
    if (f >= -2147483648.0f && f < 2147483648.0f)
      emit_f(FPOP_F2I, fpool[i], 0, (uint32_t)(int32_t)f, 0);
    if (f >= 0.0f && f < 4294967296.0f)
      emit_f(FPOP_F2U, fpool[i], 0, (uint32_t)f, 0);
  }
}

int main(void)
{
  size_t i, j;
  const size_t nd = sizeof(dpool) / sizeof(dpool[0]);
  const size_t nf = sizeof(fpool) / sizeof(fpool[0]);
  const int bin_ops[] = {FPOP_ADD, FPOP_SUB, FPOP_MUL, FPOP_DIV};
  size_t k;

  /* The arithmetic cross product is the dominant table term, so it runs over a
   * reduced "core" prefix of the pool rather than all of it.  The pools are
   * ordered specials-first, so the prefix still covers every IEEE class
   * (zeros, denormals, normals, inf, NaN) — the entries it drops are extra
   * normal values, which the random vectors cover far more cheaply.
   * CMPALL and the conversions keep the full pool: comparisons are where NaN
   * handling bites, and that path is brand new on the DCP. */
  const size_t nd_core = 18;
  const size_t nf_core = 18;

  printf("/* GENERATED by tests/fp/gen_fp_vectors.c -- do not edit.\n");
  printf(" * Regenerate with:\n");
  printf(" *   cc -O2 -o /tmp/genfp tests/fp/gen_fp_vectors.c -lm \\\n");
  printf(" *     && /tmp/genfp > tests/fp/fp_vectors.h\n");
  printf(" */\n\n");
  printf("#ifndef FP_VECTORS_H\n#define FP_VECTORS_H\n\n");
  printf("#include <stdint.h>\n\n");
  printf("#include \"fp_conformance.h\"\n\n");

  /* ── double table ── */
  printf("static const fp_vec64 fp_vectors_d[] = {\n");

  for (k = 0; k < sizeof(bin_ops) / sizeof(bin_ops[0]); k++)
    for (i = 0; i < nd_core; i++)
      for (j = 0; j < nd_core; j++)
        do_d_op(bin_ops[k], dpool[i], dpool[j]);

  for (i = 0; i < nd; i++)
    for (j = 0; j < nd; j++)
      do_d_op(FPOP_CMPALL, dpool[i], dpool[j]);

  for (i = 0; i < nd; i++)
    do_d_op(FPOP_NEG, dpool[i], 0);

  emit_d_conversions();

  /* random bit patterns, biased toward exponents that keep results finite */
  for (i = 0; i < 512; i++)
  {
    uint64_t a = rng_next(), b = rng_next();
    /* clamp exponents into [0x380, 0x430] so add/mul rarely overflows and we
     * spend the vectors on mantissa rounding instead of saturation */
    a = (a & 0x800fffffffffffffull) | ((uint64_t)(0x380 + (rng_next() % 0xb0)) << 52);
    b = (b & 0x800fffffffffffffull) | ((uint64_t)(0x380 + (rng_next() % 0xb0)) << 52);
    do_d_op(bin_ops[i % 4], a, b);
  }

  printf("};\n\n");

  /* ── float table ── */
  printf("static const fp_vec32 fp_vectors_f[] = {\n");

  for (k = 0; k < sizeof(bin_ops) / sizeof(bin_ops[0]); k++)
    for (i = 0; i < nf_core; i++)
      for (j = 0; j < nf_core; j++)
        do_f_op(bin_ops[k], fpool[i], fpool[j]);

  for (i = 0; i < nf; i++)
    for (j = 0; j < nf; j++)
      do_f_op(FPOP_CMPALL, fpool[i], fpool[j]);

  for (i = 0; i < nf; i++)
    do_f_op(FPOP_NEG, fpool[i], 0);

  emit_f_conversions();

  for (i = 0; i < 512; i++)
  {
    uint32_t a = (uint32_t)rng_next(), b = (uint32_t)rng_next();
    a = (a & 0x807fffffu) | ((uint32_t)(0x40 + (rng_next() % 0x7e)) << 23);
    b = (b & 0x807fffffu) | ((uint32_t)(0x40 + (rng_next() % 0x7e)) << 23);
    do_f_op(bin_ops[i % 4], a, b);
  }

  printf("};\n\n");

  printf("#define FP_VECTORS_D_COUNT %d\n", dcount);
  printf("#define FP_VECTORS_F_COUNT %d\n", fcount);
  printf("\n#endif /* FP_VECTORS_H */\n");

  fprintf(stderr, "generated %d double vectors, %d float vectors\n", dcount, fcount);
  return 0;
}
