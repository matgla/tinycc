/* Fuzz regression (bitfield seeds 11840/11743/15654; O2 miscompile), reduced:
 * loop_const_sim's stack-memory map keys slots by exact offset with no width
 * awareness: a packed-bitfield byte store (b3 at word+3) seeded slot(-1) but
 * left the word slot(-4) "known == 0", so the simulator collapsed the b1 RMW
 * loop into a full-word constant store that wiped b3 back to 0.
 * Fix: lcs_mem_clobber_overlaps() — any store invalidates other tracked
 * slots whose byte ranges overlap it (pre-loop scan, indirect seeding, and
 * in-simulation stores). */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
};
struct BF {
  unsigned b0 : 4;
  unsigned b1 : 2;
  unsigned b2 : 1;
};
struct BFP {
  unsigned b0 : 7;
  unsigned b1 : 5;
  unsigned b2 : 13;
  unsigned b3 : 3;
} __attribute__((packed));
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u5 = 3593363614u;
  unsigned u6 = 1719600171u;
  struct BFP bf7 = { 0u, 0u, 0u, 0u };
  struct BF bf8 = { 0u, 0u, 0u };
  bf7.b3 = (unsigned)(u5) & ((1u << 3) - 1u);
  { unsigned g10 = 0u;
    while (g10 < 10u) {
      bf7.b1 = (unsigned)(u6) & ((1u << 5) - 1u);
      g10++;
    }
  }
  cs = csmix(cs, bf7.b2);
  cs = csmix(cs, bf7.b3);
  printf("checksum=%08x\n", cs);
  return 0;
}
