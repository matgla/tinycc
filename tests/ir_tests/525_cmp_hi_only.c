/* A 64-bit compare against a constant whose LOW half is zero
 * (source/opt/flat/fusion/cmp_hi_only.c).
 *
 * The pass annotates such a compare as high-words-only, and codegen then drops
 * the `cmp rlo, #0` and turns the borrow-folding `sbcs rt, rhi, #imm_hi` into a
 * plain flag-setting `cmp rhi, #imm_hi`.  A mistake is a branch that goes the
 * wrong way, so every case below is a printed answer with host gcc as the
 * oracle -- there is nothing to crash on.
 *
 * The values that matter are the TIES: a v whose high word EQUALS the
 * constant's, with a non-zero low word.  That is the only place the low
 * comparison could have changed the answer, and the proof is that against a
 * zero low half it cannot: `v < C` is already settled false by the high words
 * being equal, because v's low word cannot be below zero.  `hi_tie_*` and the
 * `0x0000000100000001` / `0x00000000FFFFFFFF` seeds are those cases.
 *
 * The refusals are the other half of the test.  `>` and `<=` are NOT covered by
 * the proof -- `v > C` is true when the high words are equal and v's low word
 * is merely non-zero, which a high-word compare cannot see -- and neither are
 * `==`/`!=`, a constant with a non-zero low half, or a constant on the left,
 * where the mirrored condition is the non-strict one.  Each has a case here and
 * each must still give gcc's answer. */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef long long s64;

volatile u64 sink;

#define NORM ((u64)1 << 55)  /* what sfp_round_pack_double actually guards on */

int main(void)
{
  static const u64 seeds[] = {
      0ULL,
      1ULL,
      0x00000000FFFFFFFFULL, /* high word 0, low word saturated */
      0x0000000100000000ULL,
      0x0000000100000001ULL, /* tie against 1<<32, low word non-zero */
      0x007FFFFFFFFFFFFFULL,
      0x0080000000000000ULL, /* exactly NORM */
      0x0080000000000001ULL, /* tie against NORM, low word non-zero */
      0x00FFFFFFFFFFFFFFULL,
      0x0100000000000000ULL, /* exactly NORM << 1 */
      0x0100000000000001ULL, /* tie against NORM << 1 */
      0x8000000000000000ULL, /* negative as s64 */
      0x8000000000000001ULL,
      0xFFFFFFFFFFFFFFFFULL,
  };
  unsigned i;
  for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
  {
    u64 v = seeds[i];
    s64 s = (s64)v;

    /* The two shapes the pass exists for: strict <, and >=, against a constant
     * whose low half is zero.  These are round_pack's normalisation guards. */
    int a1 = v < NORM;
    int a2 = v >= NORM;
    int a3 = v < (NORM << 1);
    int a4 = v >= (NORM << 1);

    /* Ties: the high words are equal and the low word is not zero. */
    int b1 = v < 0x0000000100000000ULL;
    int b2 = v >= 0x0000000100000000ULL;
    int b3 = v < 0x0080000000000000ULL;
    int b4 = v >= 0x0080000000000000ULL;

    /* Signed order.  A 64-bit compare orders the high words signed and the low
     * words unsigned, so the same proof holds and the high-word CMP feeds the
     * same flags to a signed branch. */
    int c1 = s < (s64)0x0080000000000000LL;
    int c2 = s >= (s64)0x0080000000000000LL;
    int c3 = s < (s64)0xFF00000000000000LL; /* negative constant */
    int c4 = s >= (s64)0xFF00000000000000LL;

    /* REFUSALS.  `>` and `<=` cannot narrow to the high words: with equal high
     * words the answer turns on the low word being non-zero. */
    int d1 = v > 0x0000000100000000ULL;
    int d2 = v <= 0x0000000100000000ULL;
    int d3 = v > 0x0080000000000000ULL;
    int d4 = v <= 0x0080000000000000ULL;

    /* Equality needs both halves. */
    int e1 = v == 0x0080000000000000ULL;
    int e2 = v != 0x0000000100000000ULL;

    /* A constant with a non-zero LOW half must keep its low comparison. */
    int f1 = v < 0x0080000000000001ULL;
    int f2 = v >= 0x00000001FFFFFFFFULL;

    /* Constant on the LEFT: the mirrored conditions are the non-strict ones,
     * which the pass must refuse. */
    int g1 = 0x0080000000000000ULL < v;
    int g2 = 0x0080000000000000ULL >= v;

    printf("%u %d%d%d%d %d%d%d%d %d%d%d%d %d%d%d%d %d%d %d%d %d%d\n", i, a1, a2,
           a3, a4, b1, b2, b3, b4, c1, c2, c3, c4, d1, d2, d3, d4, e1, e2, f1,
           f2, g1, g2);
    sink = v;
  }

  /* The loop shape the pass was written for: the compare is the latch of a
   * `while`, so it is annotated once and executed many times.  Both directions,
   * exactly as sfp_round_pack_double normalises a mantissa. */
  {
    u64 m = 0x0123456789ABCDEFULL;
    int e = 0;
    while (m >= (NORM << 1))
    {
      m = (m >> 1) | (m & 1);
      e++;
    }
    while (m < NORM)
    {
      m <<= 1;
      e--;
    }
    printf("norm %llu %d\n", (unsigned long long)m, e);
    sink = m;
  }
  return 0;
}
