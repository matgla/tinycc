/* Fuzz bitfield seed 148: the known_bits pass must invalidate every tracked
 * stack slot a narrow store *overlaps*, not just the slot at the exact same
 * offset.
 *
 * `bf12` is a packed bitfield struct living in one 4-byte stack word.  After
 * the `{0}` zero-init, known_bits records that whole word slot (offset N) as a
 * fully-known constant 0.  `bf12.b3` (an 8-bit field at bits 19-26) lowers to a
 * narrow INT16 store to the *high half* of the word — a plain STORE to a
 * different stack offset (N+2).  The known_bits plain-STORE handler only
 * set/invalidated the slot at the exact store offset, so it created a fresh
 * slot for N+2 and left the N word slot still marked "known == 0".
 *
 * `bf12.b1` (bits 3-15) then lowers to a full-word read-modify-write
 * (`(*word & ~mask) | (b1<<3)`).  known_bits folded that word LOAD to the stale
 * 0 — silently dropping the b3 bits already written into the high half — so the
 * final word held only b1 and the b3 contribution to the checksum was lost.
 *
 * Fix: the plain-STORE path now invalidates any other tracked slot whose byte
 * range overlaps [off, off+width), mirroring the STORE_INDEXED / wide-store
 * paths.  (The array + loop keep bf12 stack-resident so the slot tracking that
 * the bug needs actually fires.)
 *
 * Wrong (O1 / before fix): checksum=5b8cb3b2  (b3 bits dropped)
 * Correct (O0 / O2 / fixed): checksum=faaabbd5
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

struct BFP {
  unsigned b0 : 3;
  unsigned b1 : 13;
  unsigned b2 : 3;
  unsigned b3 : 8;
  unsigned b4 : 5;
} __attribute__((packed));

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u6 = 1069483754u;
  unsigned u7 = 3551824974u;
  unsigned arr8[8] = { 1862545468u, 3126630453u, 1615973454u, 587346774u,
                       1890333011u, 1217742842u, 786072403u, 1037345132u };
  struct BFP bf12 = { 0 };

  cs = csmix(cs, u7);
  bf12.b3 = (unsigned)(723081737u * 1921231259u) & ((1u << 8) - 1u);
  bf12.b1 = (unsigned)(-(~3629639580u)) & ((1u << 13) - 1u);

  cs = csmix(cs, u6);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, bf12.b0);
  cs = csmix(cs, bf12.b1);
  cs = csmix(cs, bf12.b2);
  cs = csmix(cs, bf12.b3);
  cs = csmix(cs, bf12.b4);
  printf("checksum=%08x\n", cs);
  return 0;
}
