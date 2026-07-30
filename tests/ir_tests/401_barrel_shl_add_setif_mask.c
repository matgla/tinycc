/* Guard for three -O2 codegen levers that all fire on the same shapes:
 *
 *  1. barrel_shift (source/opt/flat/fusion/barrel_shift.c) now folds
 *     `t = x SHL #n; y = z ADD t` into `add.w y, z, x, lsl #n`.  It must NOT
 *     do so when the ADD's result reaches a dereference, or when the scale is
 *     1..3 -- both are scaled addressing modes the selector collapses into a
 *     single `ldr rd,[base,idx,lsl #n]`, and eating the shift costs one
 *     instruction per access instead of saving one.  hash_round() exercises
 *     the profitable form (shift #6, arithmetic consumer); idx_sum() and
 *     scaled_store() exercise both must-not-fuse forms.
 *
 *  2. known_bits (source/opt/flat/scalar/known_bits.c) seeds a SETIF result as
 *     0-or-1 and strips an `& M` whose mask covers every bit that can be set,
 *     so setif_branch_fuse collapses CMP+SETIF+TEST_ZERO+JUMPIF into a plain
 *     conditional branch.  bool_guard() / mask_chain() pin that; mask_chain
 *     also pins the complementary `x & M == 0` fold (mask misses bit 0).
 *
 *  3. ssa:fold's AND-mask identity, the same rewrite for the shape SSA
 *     const-prop forms after the flat pass has run (late_guard).
 *
 * Every value is checksummed, so a fold that changes semantics -- a dropped
 * shift, a wrong barrel amount, an inverted branch -- changes the output.
 */
#include <stdio.h>

/* SHL #6 feeding an arithmetic ADD: the profitable fusion. */
static unsigned hash_round(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

/* base + (i << 2) feeding a load: must stay a scaled addressing mode. */
static unsigned idx_sum(const unsigned *a, int n)
{
  unsigned s = 0;
  for (int i = 0; i < n; i++)
    s += a[i] ^ (unsigned)i;
  return s;
}

/* base + (i << 1) feeding a store: same, on the store side. */
static void scaled_store(unsigned short *d, int n, unsigned seed)
{
  for (int i = 0; i < n; i++)
    d[i] = (unsigned short)(seed + (unsigned)i * 7u);
}

/* CMP + SETIF + `& 1u` + branch: the mask is an identity on a 0/1 value. */
static unsigned bool_guard(unsigned a, unsigned b, unsigned h)
{
  if ((unsigned)(a < b) & 1u)
    h = hash_round(h, 0xAAAAAAAAu);
  else
    h = hash_round(h, 0x55555555u);
  return h;
}

/* Mask covering bit 0 -> identity; mask missing it -> the whole AND is zero. */
static unsigned mask_chain(unsigned a, unsigned b)
{
  unsigned eq = (unsigned)(a == b);
  unsigned keep = eq & 0xFFu;         /* identity: eq is 0 or 1 */
  unsigned gone = eq & 0xFFFFFF00u;   /* always 0 */
  return keep * 3u + gone;
}

/* Same identity, but the mask only becomes constant after SSA const-prop. */
static unsigned late_guard(unsigned a, unsigned b, unsigned m)
{
  unsigned lt = (unsigned)(a > b);
  return (lt & m) ? 0x1234u : 0x5678u;
}

int main(void)
{
  unsigned data[16];
  unsigned short out[16];
  unsigned cs = 0x12345678u;

  for (int i = 0; i < 16; i++)
    data[i] = (unsigned)i * 2654435761u + 0x9e3779b9u;

  cs = hash_round(cs, idx_sum(data, 16));
  scaled_store(out, 16, cs);
  for (int i = 0; i < 16; i++)
    cs = hash_round(cs, out[i]);

  cs = bool_guard(3u, 9u, cs);
  cs = bool_guard(9u, 3u, cs);
  cs = hash_round(cs, mask_chain(5u, 5u));
  cs = hash_round(cs, mask_chain(5u, 6u));
  cs = hash_round(cs, late_guard(9u, 3u, 1u));
  cs = hash_round(cs, late_guard(3u, 9u, 1u));

  /* Shift amounts across the whole barrel range, arithmetic consumers only. */
  for (unsigned k = 0; k < 32u; k++)
    cs = hash_round(cs, cs + (data[k & 15u] << k));

  printf("checksum=%08x\n", cs);
  return 0;
}
