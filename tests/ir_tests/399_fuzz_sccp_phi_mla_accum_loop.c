/* Fuzz seed bitfield:88932 (-O1/-O2 wrong code) — same root cause as test 398,
 * with the MLA in a loop body instead of straight-line code.
 *
 * `g12 * u8 + u8` fuses into `MLA src1=g12, src2=u8, accum=u8`; u8 is the
 * diamond phi of the guarded assignment below.  The guard multiplies two SETIF
 * results, one of them fed by a ternary phi, so SCCP cannot fold it until a
 * later pass constant-folds the comparisons — one round after MLA fusion.
 * sccp_materialize_const_phis() then deleted the (now CONST) phi while
 * rewriting only src1/src2, leaving
 *   T37 <-- T35 MLA #-1359611905 + T49          <- T49 has no def
 * so the loop accumulated an undefined register.
 * Correct = gcc -m32 -funsigned-char = tcc -O0. */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u7 = 3579353972u;
  unsigned u8 = 2934301684u;

  if ((((unsigned)(u7 > (u8 ^ cs)) *
        (unsigned)((~((u8 & 1u) ? u8 : 3378412899u)) != (u8 ^ cs))) & 1u) != 0u)
    u8 = 3579353972u;

  for (unsigned g12 = 0u; g12 < 11u; g12++) {
    cs = csmix(cs, g12);
    cs += g12 * u8 + u8;
  }

  printf("checksum=%08x\n", cs);
  return 0;
}
