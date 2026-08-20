/* Narrowing a 64-bit compare whose operands provably have zero high words
 * (source/opt/flat/fusion/cmp_narrow_64.c).
 *
 * The pass rewrites a CMP's operands from 64-bit to 32-bit, so a mistake is a
 * branch that goes the wrong way -- there is nothing to crash on.  Three of
 * its rules are exercised here:
 *
 *  - AND with a mask whose high word is zero.  `rem = mant & 7; rem > 4` is
 *    sfp_round_pack_double's rounding decision, and as a 64-bit compare it
 *    costs two instructions to build the 64-bit 4, a CMP and an SBCS.
 *  - SHR by >= 32, and ZEXT from u32.
 *  - Either operand may be the constant: the frontend swaps the operands of a
 *    UGT/ULE compare to reach the condition codes the backend has, so the
 *    value can arrive as src2.
 *
 * `signed_order` is the refusal that matters most: a u64 with a zero high word
 * can be positive at 64 bits and NEGATIVE as an int32, so a signed comparison
 * must keep its width.  `addr_taken` and `two_defs` pin the named-local rules
 * -- one definition and no address handed out -- since unlike a temp a local
 * can be written somewhere this pass cannot see.
 *
 * Expected output comes from host gcc, so it is a real oracle. */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef long long s64;
typedef unsigned u32;

volatile u64 sink;

static void take(u64 *p) { *p |= 0x100000000ULL; }

int main(void)
{
  static const u64 seeds[] = {0ULL, 1ULL, 3ULL, 4ULL, 5ULL, 7ULL, 8ULL,
                              0x00000000FFFFFFFFULL, 0x0000000080000000ULL,
                              0xFFFFFFFFFFFFFFFFULL, 0x123456789ABCDEFULL};
  unsigned i;
  for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
  {
    u64 m = seeds[i];

    /* AND with a small mask, both operand orders and both strictnesses */
    u64 rem = m & 7;
    int a1 = rem > 4;
    int a2 = rem >= 4;
    int a3 = 4 < rem;
    int a4 = rem == 4;
    int a5 = rem != 4;

    /* SHR by >= 32 and ZEXT */
    u64 hi = m >> 32;
    int b1 = hi > 3;
    int b2 = hi == 0;
    u64 z = (u64)(u32)m;
    int b3 = z > 0x7FFFFFFFULL;
    int b4 = z == 0xFFFFFFFFULL;

    /* signed order on a value whose high word is zero: as an int32 the same
     * bits can be negative, so this must NOT narrow */
    s64 sv = (s64)(m & 0xFFFFFFFFULL);
    int c1 = sv > 100;
    int c2 = sv < 0;

    /* a named local whose address escapes, and one written twice */
    u64 esc = m & 0xFF;
    if (i == 3)
      take(&esc);
    int d1 = esc > 4;

    u64 two = m & 0xF;
    if (i & 1)
      two = 0x200000000ULL;
    int d2 = two > 4;

    printf("%u %d%d%d%d%d %d%d%d%d %d%d %d %d\n", i, a1, a2, a3, a4, a5,
           b1, b2, b3, b4, c1, c2, d1, d2);
    sink = rem + hi + z + esc + two;
  }
  return 0;
}
