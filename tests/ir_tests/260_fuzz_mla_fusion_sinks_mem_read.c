#include <stdio.h>

/*
 * Fuzz volatile seed 5053 reduction (O2): ssa_gen_arm_fuse_mul_add_to_mla
 * places the fused MLA at the ADD's position, which re-reads the MUL's
 * operands there.  Here the MUL's src1 was a fused memory read of st12.f0's
 * stack slot (vv11 = st12.f0 * u5 before the g23 loop); the ADD consuming
 * the product sits after the loop, and the loop stores a new value to
 * st12.f0 — so the sunk load read the updated slot instead of the
 * pre-loop value.  Fix: when a MUL source operand reads memory (is_lval /
 * is_llocal), only fuse if the def→use range is straight-line and free of
 * stores/calls; mirrored guard for the accumulator hoist in the pre-SSA
 * ir_gen_mla_fusion.
 * Expected checksum (gcc -O2 arm-none-eabi + tcc -O0/-O1/-Os): a0831b36.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(lr) & (unsigned)((-((unsigned)(pb) | 0u))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s2 = (char)(1270340573u & 0xff);
  unsigned u3 = 3668163544u;
  unsigned u4 = 2913798353u;
  unsigned u5 = 1491045512u;
  unsigned u6 = 1805048137u;
  unsigned u7 = 2648717802u;
  unsigned u8 = 1927646133u;
  volatile unsigned vv9 = 1672572869u;
  volatile unsigned vv10 = 811077700u;
  volatile unsigned vv11 = 2182386783u;
  struct S st12 = { 3543778724u, 1774806629u, 2155983143u };
  struct S st13 = { 1042155439u, 782028733u, 2298297281u };
  cs = csmix(cs, vv11);
  cs = csmix(cs, vv10);
  { unsigned g15 = 0u;
    while (g15 < 8u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      cs = csmix(cs, vv10);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(3696639582u) ^ (unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(st12.f2))))) & (unsigned)(((unsigned)(((unsigned)(st12.f0) * (unsigned)(((unsigned)(u8) * (unsigned)(u6))))) >> ((unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)(((unsigned)(141155289u) <= ((unsigned)((unsigned)(s2)) ^ cs))) : (unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(u8))))) & 31u))))));
      g15++;
    }
  }
  if ((unsigned)(897243250u) & 1u) {
    { unsigned g17 = 0u;
      while (g17 < 1u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, vv9);
        cs = csmix(cs, vv9);
      }
    }
    for (unsigned g19 = 0u; g19 < 7u; g19++) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      cs = csmix(cs, vv10);
    }
    if ((unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)(st13.f1) : (unsigned)(2754873891u))) * (unsigned)(((unsigned)(u3) >> ((unsigned)((unsigned)(s2)) & 31u))))) ^ (unsigned)(helper1(1050587407u, 1679969678u)))) << ((unsigned)(((unsigned)(u5) + (unsigned)(((unsigned)((~((unsigned)(u8) | 0u))) * (unsigned)(((unsigned)(st12.f2) + (unsigned)(u8))))))) & 31u))) & 1u) {
      cs = csmix(cs, vv11);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1729149140u) * (unsigned)(helper1(u6, ((unsigned)(905257442u) ^ (unsigned)(1185603041u)))))) / ((unsigned)(2314402712u) | 1u))));
    }
  } else {
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(2296428878u) / ((unsigned)(((unsigned)(u4) >> ((unsigned)(helper1(u8, 2172919023u)) & 31u))) | 1u))) >= ((unsigned)((-((unsigned)(((unsigned)(((unsigned)(3037382120u) > ((unsigned)(u7) ^ cs))) ^ (unsigned)((((unsigned)(1951369261u) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(u3))))) | 0u))) ^ cs))));
    vv11 = (unsigned)(((unsigned)(st12.f0) * (unsigned)(u5)));
    for (unsigned g21 = 0u; g21 < 6u; g21++) {
      unsigned i20 = g21;
      cs = csmix(cs, i20);
    }
  }
  { unsigned g23 = 0u;
    while (g23 < 12u) {
      unsigned i22 = g23;
      cs = csmix(cs, i22);
      for (unsigned g25 = 0u; g25 < 8u; g25++) {
        unsigned i24 = g25;
        cs = csmix(cs, i24);
        cs = csmix(cs, (unsigned)(u6));
      }
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(((unsigned)(u3) % ((unsigned)(3025596070u) | 1u))) * (unsigned)((-((unsigned)(654840605u) | 0u))))) | 0u))) & (unsigned)(1988128467u))));
      st12.f0 = (unsigned)((((unsigned)(351474275u) & 1u) ? (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(helper1(3241771840u, 2418725234u)) & 31u))) : (unsigned)(((unsigned)(((unsigned)(((unsigned)(2873443444u) - (unsigned)(st13.f1))) | (unsigned)((unsigned)(s2)))) / ((unsigned)(((unsigned)(((unsigned)(u8) % ((unsigned)(3017729302u) | 1u))) >> ((unsigned)(u6) & 31u))) | 1u)))));
      g23++;
    }
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, vv9);
  cs = csmix(cs, vv10);
  cs = csmix(cs, vv11);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  cs = csmix(cs, st13.f0);
  cs = csmix(cs, st13.f1);
  cs = csmix(cs, st13.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
