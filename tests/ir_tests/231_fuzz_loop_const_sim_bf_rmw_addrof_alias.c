/* Fuzz bitfield seed 5: loop constant-simulation must seed a stack slot's
 * pre-loop value from indirect stores through an address-of alias, not only
 * from direct StackLoc stores.
 *
 * b1 is written before the loop via a packed-bitfield RMW, which lowers to an
 * indirect store through Addr[StackLoc] (`T = Addr[bf]; *T = 38`).  The loop
 * body RMWs b2 (a different field of the SAME storage word).  loop_const_sim
 * collapses the fixed-trip loop to a residual store; its pre-loop scan only
 * recognised *direct* StackLoc stores when seeding the slot's initial value,
 * so it missed the b1 store and simulated the word from the stale initializer
 * value 0.  The residual store then wrote (0 & ~b2mask)|b2 -- clobbering b1
 * back to 0.  Fix: the pre-loop scan resolves indirect stores through a known
 * Addr[StackLoc] temp/var to the same slot the body simulator uses.
 *
 * Wrong (O1/O2 before fix): checksum=1234569a  (b1 reads back 0)
 * Correct (O0 / fixed):      checksum=123456ad  (b1 reads back 19)
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}

struct BFP {
  unsigned b0 : 1;
  unsigned b1 : 5;
  unsigned b2 : 6;
  unsigned b3 : 11;
  unsigned b4 : 5;
} __attribute__((packed));

int main(void)
{
  unsigned cs = 0x12345678u;
  char s4 = (char)(767293282u & 0xff);
  unsigned u7 = 1175468587u;
  struct BFP bf12 = { 0u, 0u, 0u, 0u, 0u };
  bf12.b1 = 19u;
  unsigned g14 = 0u;
  while (g14 < 7u) {
    unsigned i13 = g14;
    cs += i13 / u7;
    bf12.b2 = (unsigned)((unsigned)(s4)) & ((1u << 6) - 1u);
    g14++;
  }
  cs += bf12.b1;
  cs += bf12.b2;
  printf("checksum=%08x\n", cs);
  return 0;
}
