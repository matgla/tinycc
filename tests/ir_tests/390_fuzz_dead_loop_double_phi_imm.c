/* Fuzz seed fp_round:46 (-O2): ssa:dead_loop's invariant-phi substitution
 * skipped INT64 phis (irop_make_imm32 holds 32 bits) but NOT FLOAT64 — a
 * loop-invariant `f7 = -(-c)` double latch constant was truncated to a
 * TAG_IMM32 and substituted into f7's uses, garbling the double.  Fix: skip
 * FLOAT64/FLOAT32 phis too. */
#include <stdio.h>
#include <string.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned fbits_d(double d){ unsigned u[2]; memcpy(u, &d, sizeof u); return csmix(u[0], u[1]); }
int main(void)
{
  unsigned cs = 0x12345678u;
  double f7 = 0x1.c007280000000p+36;
  { unsigned g12 = 0u;
    while (g12 < 5u) {
      cs = csmix(cs, g12);
      f7 = -(-0x1.94a79c0000000p+25);
      g12++;
    }
  }
  cs = csmix(cs, fbits_d(f7));
  printf("checksum=%08x\n", cs);
  return 0;
}
