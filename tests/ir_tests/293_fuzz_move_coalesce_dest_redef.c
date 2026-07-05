/*
 * bitfield fuzz seed 40979 reduction (O1/O2 wrong-code):
 * post-RA reverse move coalescing (ir/regalloc.c tcc_ir_move_coalescing)
 * merged the copy `u4 = u3` (V2 <- V1) by reassigning src V1 onto dest V2's
 * register R5. It only checked that SRC was not redefined while dest is live;
 * it never checked the symmetric case -- that DEST is not redefined while SRC
 * is still live. Here `u4` (dest) is reassigned to a constant a few lines
 * later while `u3` (src) is still read, so the `u4 = const` write clobbered
 * the shared R5 and the csmix(cs, u3) argument read the constant instead of
 * u3's value. The reverse pass normally targets loop-carried phi copies where
 * src dies AT the copy, so the new dest-redefinition guard's scan range is
 * empty there and legitimate coalescing is unaffected.
 * Ground truth (tcc -O0 == tcc -O2/-Os == arm-none-eabi-gcc -O2): d06114c4.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

struct BF {
  unsigned b0 : 5;
  unsigned b1 : 11;
  unsigned b2 : 8;
  unsigned b3 : 1;
  unsigned b4 : 2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u3 = 3866678625u;
  unsigned u4 = 2928217611u;
  unsigned arr5[8] = { 1514363860u, 1717216661u, 706011553u, 251047386u,
                       925390689u, 1833920460u, 1326246819u, 788789791u };
  struct BF bf8 = { 0u, 0u, 0u, 0u, 0u };

  u3 = (unsigned)(((unsigned)(u4) | (unsigned)(((unsigned)(arr5[((unsigned)(u4) & 7u)]) & (unsigned)(1728505800u))))) & 0xffffffffu;
  u4 = (unsigned)(u3) & 0xffffffffu;
  /* intervening read of u4 keeps u3 (its copy source) live past the copy */
  bf8.b0 = (unsigned)(arr5[((unsigned)(u4) & 7u)]) & ((1u << 5) - 1u);
  u4 = (unsigned)(1459660296u) & 0xffffffffu;

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr5[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
