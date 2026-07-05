#include <stdio.h>

/*
 * Fuzz ptr seed 8507 reduction (O2): ssa:load_cse tracked `*T = const`
 * (T = Addr[StackLoc[-32]] + 4, i.e. &arr8[1], unresolved because the
 * stack-LEA resolver only chases vreg src1 in ADD chains) as a TVStore,
 * then the direct stack store `StackLoc[-28] <- u2` (arr8[u6&7] = u2,
 * same address) did not invalidate TVStore entries — the STACKOFF-dest
 * store branch only maintains sstores.  The later read of *p9 in the
 * csmix argument was forwarded the stale constant 3581582797 instead of
 * u2.  Fix: direct stack stores (plain and indexed) and global-sym
 * stores drop all TVStore entries, since a TVStore pointer is by
 * construction unresolved and may alias either class.
 */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)(228259646u & 0xffffffff);
  unsigned u2 = 1197074507u;
  unsigned u3 = 4020882957u;
  unsigned u4 = 3042415691u;
  unsigned u5 = 1453426358u;
  unsigned u6 = 1472990201u;
  unsigned u7 = 3850786480u;
  unsigned arr8[8] = { 3147441909u, 3589539834u, 920354924u, 3686816661u, 2913706472u, 3077811734u, 977559817u, 1808091898u };
  unsigned *p9 = &arr8[((unsigned)(u6) & 7u)];
  unsigned *p10 = &arr8[((unsigned)(u6) & 7u)];
  unsigned *p11 = &u4;
  unsigned *p12 = &arr8[((unsigned)(u6) & 7u)];
  struct S st13 = { 3251631936u, 3779399586u, 2498625269u };
  struct S st14 = { 2202719087u, 807211840u, 3889461394u };
  if ((unsigned)(((unsigned)((((unsigned)(((unsigned)((*p10)) % ((unsigned)(((unsigned)(st13.f2) << ((unsigned)(st14.f2) & 31u))) | 1u))) & 1u) ? (unsigned)(u5) : (unsigned)(st13.f2))) << ((unsigned)(2647373474u) & 31u))) & 1u) {
    cs = csmix(cs, *p11);
    cs = csmix(cs, *p12);
    for (unsigned g16 = 0u; g16 < 10u; g16++) {
      unsigned i15 = g16;
      cs = csmix(cs, i15);
      cs = csmix(cs, *p9);
    }
  } else {
    cs = csmix(cs, *p10);
    u2 = (unsigned)(((unsigned)(((unsigned)(st14.f2) & (unsigned)((((unsigned)((*p12)) & 1u) ? (unsigned)(u6) : (unsigned)(((unsigned)(1307369208u) + (unsigned)(3841446595u))))))) << ((unsigned)(u4) & 31u))) & 0xffffffffu;
    cs = csmix(cs, *p11);
  }
  cs = csmix(cs, *p9);
  *p9 = (unsigned)((((unsigned)((unsigned)(s1)) & 1u) ? (unsigned)(st13.f0) : (unsigned)(3581582797u)));
  cs = csmix(cs, *p12);
  arr8[((unsigned)(u6) & 7u)] = (unsigned)(u2);
  cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(((unsigned)(u6) | (unsigned)((*p9)))) | 0u))) & (unsigned)(((unsigned)(((unsigned)(((unsigned)(1830772650u) << ((unsigned)((*p12)) & 31u))) >> ((unsigned)(((unsigned)(u2) ^ (unsigned)(232235144u))) & 31u))) ^ (unsigned)(((unsigned)((unsigned)(s1)) >> ((unsigned)(((unsigned)(548846994u) / ((unsigned)(1839874110u) | 1u))) & 31u))))))));
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st13.f0);
  cs = csmix(cs, st13.f1);
  cs = csmix(cs, st13.f2);
  cs = csmix(cs, st14.f0);
  cs = csmix(cs, st14.f1);
  cs = csmix(cs, st14.f2);
  cs = csmix(cs, *p9);
  cs = csmix(cs, *p10);
  cs = csmix(cs, *p11);
  cs = csmix(cs, *p12);
  printf("checksum=%08x\n", cs);
  return 0;
}
