/*
 * Bug: 64-bit multiply cross-term uses wrong register in
 *      thumb_emit_regonly_binop32 when scratch allocator reuses an
 *      already-occupied source register.
 *
 * In parse_number() (tccpp.c), the loop:
 *   unsigned long long n = 0;
 *   int b = 10;  // decimal base
 *   n = n * b + t;
 *
 * The 64-bit multiply is decomposed into:
 *   UMULL  (n_lo * b → ML:MH)
 *   MUL    (n_hi * b → cross1)     ← bug here
 *   MUL    (0 * n_lo → cross2)
 *   ADD    (MH + cross1 + cross2)
 *
 * In thumb_emit_regonly_binop32(), when src1 (n_hi) already occupies a
 * physical register, get_scratch_reg_with_save() for src2 (b) does not
 * exclude src1's register from the candidate set. Under high register
 * pressure (R9 reserved as GOT pointer + many live locals), the scratch
 * allocator picks the SAME register for src2 as src1 already uses.
 *
 * Result: MUL Rd, Rm, Rm (b*b=100) instead of MUL Rd, Rn, Rm (n_hi*b).
 * This leaves 0x64 in the upper 32 bits of n, causing "constant exceeds
 * 32 bit" when the value is later used as an array dimension.
 *
 * This test reproduces the parse_number integer accumulation path with
 * enough register pressure to force the scratch allocator collision.
 * Several volatile variables simulate live-across-loop state.
 * Compile with -mpic-data-is-text-relative to reserve R9 (GOT),
 * matching the yasos native build environment.
 */
#include <stdio.h>

/* Prevent the compiler from constant-folding the base away */
static int __attribute__((noinline)) get_base(void)
{
  return 10;
}
static const char *__attribute__((noinline)) get_digits(const char *p)
{
  return p;
}

/*
 * parse_number-like integer accumulation with high register pressure.
 *
 * Variables: p (pointer being advanced), q (second pointer), b (base),
 * t (digit), ch (lookahead char), n (64-bit accumulator), n1 (previous n),
 * ov (overflow flag), lcount, ucount — all live across the loop.
 * This mirrors the actual variable set in tccpp.c:parse_number().
 */
static unsigned long long parse_int(const char *p, int b)
{
  unsigned long long n, n1;
  int t, ov = 0;
  const char *q;
  int ch, lcount, ucount;

  /* Phase 1: scan digits into buffer (keeps q, ch, p live) */
  q = p;
  ch = *q++;
  while (ch >= '0' && ch <= '9')
  {
    ch = *q++;
  }

  /* Phase 2: integer accumulation (the buggy code path) */
  /* Mimic parse_number exactly: re-parse from token_buf (q = p) */
  q = p;
  n = 0;
  lcount = 0;
  ucount = 0;
  while (1)
  {
    t = *q++;
    if (t == '\0')
      break;
    else if (t >= 'a')
      t = t - 'a' + 10;
    else if (t >= 'A')
      t = t - 'A' + 10;
    else
      t = t - '0';
    if (t >= b)
      break;
    n1 = n;
    n = n * b + t; /* THE critical multiply */
    /* overflow check keeps n1, n, b all live simultaneously */
    if (n1 >= 0x1000000000000000ULL && n / b != n1)
      ov = 1;
  }

  /* Phase 3: suffix parsing (keeps ch, p, lcount, ucount live) */
  ch = *q;
  if (ch == 'L' || ch == 'l')
  {
    lcount++;
    ch = *++q;
    if (ch == 'L' || ch == 'l')
    {
      lcount++;
      ch = *++q;
    }
  }
  if (ch == 'U' || ch == 'u')
  {
    ucount++;
  }

  /* Determine 64bit/unsigned (keeps n, lcount, ucount, ov live) */
  if (ucount == 0 && b == 10)
  {
    if (lcount <= 1)
    {
      if (n >= 0x80000000U)
        lcount = 2;
    }
    if (n >= 0x8000000000000000ULL)
      ov = 1, ucount = 1;
  }

  /* Use ov, lcount, ucount to prevent dead-code elimination */
  if (ov)
    printf("overflow lcount=%d ucount=%d\n", lcount, ucount);

  return n;
}

int main(void)
{
  int base = get_base();
  unsigned long long r;
  unsigned lo, hi;

  /* 10000000001 = 0x2_540BE401 — needs correct cross-term for hi=2 */
  r = parse_int(get_digits("10000000001"), base);
  lo = (unsigned)r;
  hi = (unsigned)(r >> 32);
  printf("r1 hi=%08x lo=%08x\n", hi, lo);
  if (hi != 0x00000002 || lo != 0x540be401)
  {
    printf("FAIL r1\n");
    return 1;
  }

  /* 99999999999 = 0x17_4876E7FF — larger cross-term contribution */
  r = parse_int(get_digits("99999999999"), base);
  lo = (unsigned)r;
  hi = (unsigned)(r >> 32);
  printf("r2 hi=%08x lo=%08x\n", hi, lo);
  if (hi != 0x00000017 || lo != 0x4876e7ff)
  {
    printf("FAIL r2\n");
    return 1;
  }

  /* 1000000000000 = 0xE8_D4A51000 — even bigger hi word */
  r = parse_int(get_digits("1000000000000"), base);
  lo = (unsigned)r;
  hi = (unsigned)(r >> 32);
  printf("r3 hi=%08x lo=%08x\n", hi, lo);
  if (hi != 0x000000e8 || lo != 0xd4a51000)
  {
    printf("FAIL r3\n");
    return 1;
  }

  printf("PASS\n");
  return 0;
}
