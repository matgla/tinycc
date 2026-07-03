/* Regression: pack64-from-stack-stores folded a 64-bit LOAD using an unrelated
 * variable's stores when the u64's spill-home offset aliased a live stack slot.
 *
 * From longlong fuzz seed 7 (reduced).  tcc -O0/-O1/-O2/-Os all agreed with
 * each other but disagreed with gcc — an O0-class codegen miscompile.
 *
 * Root cause: tcc_ir_opt_pack64_from_stack_stores rewrites `T(i64) <-- LOAD
 * StackLoc[A]` into `PACK64(val_lo, val_hi)` by scanning backwards for the two
 * adjacent 32-bit stores at [A, A+4].  It matched the LOAD's source purely by
 * stack offset.  But a u64 local (`q14`) that stays register-resident carries a
 * STACKOFF operand that is only a *spill-home hint* (tag==STACKOFF, is_local,
 * is_lval, but vreg_type != 0) — the value is read from the vreg, not that slot.
 * The register allocator had reused q14's never-written spill home for `arr[0]`,
 * so the backward scan matched arr[0]/arr[1]'s stores and folded
 * `(unsigned)q14 ^ (unsigned)(q14>>32)` to `arr[0] ^ (q14>>32)` — a wrong low
 * word (0x7afb2c68 instead of u5=0x068739fa in the original seed).
 *
 * Fix: the pass now requires the LOAD source (and the matched store dests) to be
 * *direct* StackLoc references — irop_get_vreg(op) == -1 (vreg_type == 0) — per
 * the IROP_TAG_STACKOFF contract in tccir_operand.h.  A VAR/PARAM spill encoding
 * is no longer treated as a real memory read/write.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u5 = 109525498u;   /* 0x068739fa */
  unsigned u7 = 2637406236u;
  unsigned u9 = 527603371u;
  unsigned u10 = 3646156326u; /* 0xd953ee26 */
  unsigned arr[8] = { 2063281256u, 1339395518u, 618979930u, 3219824981u,
                      3179784293u, 2972361206u, 2217639874u, 1553714997u };
  /* Four 64-bit locals — enough register pressure that at least one keeps its
   * spill home unwritten and the allocator reuses it for `arr`. */
  unsigned long long q12 = (((unsigned long long)u9) << 32) | (unsigned long long)u5;
  unsigned long long q13 = (((unsigned long long)u9) << 32) | (unsigned long long)u7;
  unsigned long long q14 = (((unsigned long long)u10) << 32) | (unsigned long long)u5;
  unsigned long long q15 = (((unsigned long long)u10) << 32) | (unsigned long long)u7;

  arr[6] = u10;
  /* The xor-fold of each u64's two halves — the miscompiled read. */
  cs = csmix(cs, (unsigned)q14 ^ (unsigned)(q14 >> 32));
  cs = csmix(cs, (unsigned)q12 ^ (unsigned)(q12 >> 32));
  cs = csmix(cs, (unsigned)q13 ^ (unsigned)(q13 >> 32));
  cs = csmix(cs, (unsigned)q15 ^ (unsigned)(q15 >> 32));
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr[k]);

  printf("checksum=%08x\n", cs);
  return 0;
}
