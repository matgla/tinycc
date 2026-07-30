/* 64-bit bitfield extraction and the folds behind it (bitfld-3 gap):
 *
 *  1. `(x SHL c) SHR c` on an unsigned i64 folds to `x AND (~0ULL >> c)`
 *     (ssa:fold fold_shl_shr_mask64).  Fields of 33/40/41 bits sit at bit 0
 *     of their own 8-byte unit, so every extract is an equal-shift pair and
 *     the AND's low-word mask is all-ones — each width and a cross-term
 *     arithmetic mix is checked so a wrong mask or a dropped high word fails
 *     here, not just in the torture suite.
 *  2. SIGNED 64-bit fields extract with SHL+SAR, which must NOT fold to a
 *     mask: negative field values pin the sign-extension.
 *  3. A field that CROSSES its unit's low/high word boundary (u41b below at
 *     bit offset 20 of its unit) extracts with unequal shifts — the fold must
 *     leave it alone and the generic shift path must still be right.
 *  4. global_deref_cse now admits INT64 reads and lets a region cross a
 *     conditional exit: the same unit read on both sides of an `if` may share
 *     one load, but a STORE in between must invalidate the cached value
 *     (w.u40w write, then re-read), and a store through a helper call must
 *     flush too.
 */
#include <stdio.h>

struct s {
  unsigned long long u33 : 33;
  unsigned long long u40 : 40;
  unsigned long long u41 : 41;
};

struct t {
  signed long long s33 : 33;
  signed long long s40 : 40;
};

/* u41b starts at bit 20 of its unit: 20 + 41 <= 64 keeps it in ONE unit but
 * spanning both machine words, so the extract is an unequal-shift pair. */
struct u {
  unsigned long long pad : 20;
  unsigned long long u41b : 41;
};

struct s a = { 0x100000, 0x100000, 0x100000 };
struct s b = { 0x100000000ULL, 0x1F00000023ULL, 0x10000000101ULL };
struct t t1 = { -1, -0x2000000000LL };
struct u u1 = { 0x12345, 0x10000000001ULL };
struct s w = { 1, 2, 3 };

static unsigned long long g64 = 0x1122334455667788ULL;

void poke(void) { g64 = 0x99AABBCCDDEEFF00ULL; }

/* Opaque selector: keeps the branch below alive all the way to the allocator,
 * so the dominance rule is actually exercised rather than folded away. */
volatile int sel_v[2] = { 1, 0 };
int sel(int k) { return sel_v[k]; }

int main(void)
{
  /* 1: equal-shift folds, every width, cross-term products and sums. */
  printf("a=%llx %llx %llx\n",
         (unsigned long long)a.u33, (unsigned long long)a.u40,
         (unsigned long long)a.u41);
  printf("m=%llx %llx %llx\n",
         (unsigned long long)(a.u33 * a.u41),
         (unsigned long long)(b.u33 + b.u40),
         (unsigned long long)(a.u40 - b.u41));

  /* 2: signed fields — SHL+SAR must stay sign-extending. */
  printf("t=%lld %lld\n", (long long)t1.s33, (long long)t1.s40);
  printf("tc=%d %d\n", t1.s33 < 0, t1.s40 < -0x1000000000LL);

  /* 3: word-boundary-crossing field (unequal shifts, no fold). */
  printf("u=%llx %llx\n", (unsigned long long)u1.pad,
         (unsigned long long)u1.u41b);

  /* 4: deref-CSE region rules.  Same unit on both sides of a branch... */
  if (b.u33 != 0x100000000ULL || b.u41 != 0x10000000101ULL)
    return 1;
  /* ...then a direct store invalidates the cached unit... */
  if (w.u33 != 1)
    return 2;
  w.u33 = 0x1FFFFFFFFULL; /* writes all 33 bits */
  if (w.u33 != 0x1FFFFFFFFULL || w.u40 != 2 || w.u41 != 3)
    return 3;
  /* ...and a store behind a call flushes too. */
  if (g64 != 0x1122334455667788ULL)
    return 4;
  poke();
  printf("g=%llx\n", g64);

  /* Explicit source-level equal-shift pair on a plain u64. */
  printf("x=%llx\n", (g64 << 25) >> 25);

  /* 5: narrowing-ASSIGN CSE (gvn_try_assign_narrow).  A 64-bit multiply
   * splits each operand into low/high words via truncating copies; when one
   * operand feeds several products those copies duplicate, and the dup is
   * rewritten to copy the first narrowing so the allocator can coalesce it.
   * Three products share `p`'s halves — a wrong substitution swaps a high
   * word for a low one and the results diverge. */
  unsigned long long p = 0x0123456789ABCDEFULL;
  unsigned long long q = 0xFEDCBA9876543210ULL;
  unsigned long long r = 0x00000001DEADBEEFULL;
  printf("p3=%llx %llx %llx\n", p * q, p * r, p * p);

  /* The CSE key carries the destination's width AND signedness, so several
   * narrowings of the SAME source to different types must not merge. */
  unsigned int   n32 = (unsigned int)p;
  unsigned short n16 = (unsigned short)p;
  unsigned char  n8  = (unsigned char)p;
  signed int     s32 = (signed int)p;
  signed short   s16 = (signed short)p;
  printf("nw=%x %x %x %d %d\n", n32, n16, n8, s32, s16);

  /* Narrowing under a branch: a copy numbered inside one arm must never be
   * reused in the sibling arm, which it does not dominate.  Each arm narrows
   * a DIFFERENT source, and both arms run (the selector is volatile so the
   * branch survives to the allocator), so a cross-arm substitution prints the
   * other arm's value. */
  for (int k = 0; k < 2; k++) {
    unsigned long long v = sel(k) ? p : q;
    unsigned int lo = (unsigned int)v;
    if (sel(k)) {
      unsigned int a2 = (unsigned int)(p >> 32);
      printf("b1=%x %x\n", lo, a2);
    } else {
      unsigned int b2 = (unsigned int)(q >> 32);
      printf("b0=%x %x\n", lo, b2);
    }
  }
  return 0;
}
