/* `(v64 >> k) & mask` with k >= 32 as one UBFX on the high word
 * (source/opt/flat/fusion/shift64_extract_ubfx.c).
 *
 * Every accessor in lib/fp/soft/soft_common.h is this shape -- `(bits >> 63)
 * & 1` for the sign, `(bits >> 52) & 0x7FF` for the exponent -- and the
 * rewrite replaces a shift and a mask with a single instruction reading the
 * source's high register.  A wrong lsb or width silently returns a different
 * field, so the expected output comes from host gcc.
 *
 * `k` is swept across the whole valid range and both spellings of the
 * consumer are covered: a plain AND against a contiguous mask, and the UBFX
 * the narrowing passes make of one.  The refusals matter as much:
 *
 *  - `straddle` -- a field that crosses the word boundary is not in the high
 *    word at all, so it must keep the full 64-bit shift.
 *  - `low_shift` -- a count below 32 says nothing about which word the field
 *    is in.
 *  - `arith` -- an arithmetic shift fills from the sign bit; SAR is excluded.
 *  - `two_uses` -- the shift feeding something else as well must stay, and
 *    then folding it into the mask would only duplicate work.
 *  - `redefined` -- the UBFX reads the SOURCE at the mask's position, so a
 *    store to it in between changes what is extracted.
 */

extern int printf(const char *, ...);

typedef unsigned long long u64;
typedef unsigned u32;

volatile u64 sink;

/* The soft-float accessors, verbatim in shape. */
static u32 dsign(u64 b) { return (u32)((b >> 63) & 1); }
static u32 dexp(u64 b) { return (u32)((b >> 52) & 0x7FF); }

/* A field crossing the 32-bit boundary: must NOT become a high-word UBFX. */
static u32 straddle(u64 b) { return (u32)((b >> 28) & 0xFF); }

/* Count below 32: the field can be anywhere. */
static u32 low_shift(u64 b) { return (u32)((b >> 4) & 0x3F); }

/* Arithmetic shift: the high bits are the sign, not zero. */
static u32 arith(long long b) { return (u32)((b >> 40) & 0xFFF); }

/* The shift result read twice: it has to survive. */
static u32 two_uses(u64 b)
{
  u64 t = b >> 40;
  return (u32)(t & 0xFF) + (u32)(t >> 8);
}

/* Source rewritten between the shift and the mask. */
static u32 redefined(u64 b, int go)
{
  u64 v = b;
  u64 t = v >> 48;
  if (go)
    v = 0;
  sink = v;
  return (u32)(t & 0x1FF);
}

int main(void)
{
  static const u64 seeds[] = {0ULL,
                              1ULL,
                              0x8000000000000000ULL,
                              0x7FF0000000000000ULL,
                              0xFFF8000000000000ULL,
                              0x0123456789ABCDEFULL,
                              0xFEDCBA9876543210ULL,
                              0x00000000FFFFFFFFULL,
                              0xFFFFFFFF00000000ULL,
                              0xFFFFFFFFFFFFFFFFULL};
  unsigned i, k, w;

  for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
  {
    u64 s = seeds[i];
    printf("acc %u %u %u\n", i, dsign(s), dexp(s));
    printf("ref %u %u %u %u %u %u\n", i, straddle(s), low_shift(s), arith((long long)s), two_uses(s),
           redefined(s, (int)(i & 1)));
  }

  /* Every count from 32 to 63 against every width that still fits, both as a
   * mask and through a second extract. */
  for (i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++)
  {
    u64 s = seeds[i];
    for (k = 32; k < 64; k++)
    {
      u32 acc = 0;
      for (w = 1; w + (k - 32) <= 32; w++)
      {
        u32 mask = (w == 32) ? 0xFFFFFFFFu : ((1u << w) - 1u);
        acc += (u32)((s >> k) & mask);
        acc ^= (u32)(((s >> k) & 0xFFFFFFFFu) & mask) << 1;
      }
      printf("sweep %u %u %08x\n", i, k, acc);
    }
    sink = s;
  }
  return 0;
}
