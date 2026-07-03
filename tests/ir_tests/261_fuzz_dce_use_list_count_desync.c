#include <stdio.h>

/*
 * Fuzz ptr seed 7226 reduction (O2): two SSA use-chain bookkeeping defects
 * combined to delete a live pointer def.  (1) ssa:load_cse's STORE-src stack
 * forward folded `T <- Tptr***DEREF*** [STORE]` to an immediate without
 * removing Tptr's use record.  (2) ssa_opt_dce's repair step rebuilt only
 * use_count, not the uses[] entries; after swap-removes reordered the list,
 * the truncated prefix kept the stale entry and dropped the live deref use
 * (*p9 in the tail).  cprop's replace_all_uses then walked the wrong list,
 * left the deref un-rewritten, and DCE deleted the pointer's def — the final
 * csmix(cs, *p9) dereferenced an undefined vreg.
 * Fix: remove the use at the load_cse fold; rebuild full use lists in the
 * DCE repair (ssa_opt_scan_instr_uses).
 * Expected checksum (gcc -O2 arm-none-eabi + tcc -O0/-O1/-Os): b6640c72.
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
  long s1 = (long)(701659381u & 0xffffffff);
  short s2 = (short)(1379047210u & 0xffff);
  unsigned u3 = 1566467004u;
  unsigned u4 = 2560653898u;
  unsigned arr5[8] = { 1449684285u, 2732337238u, 4034577053u, 183465155u, 2447708742u, 3273282169u, 2172198899u, 3454644283u };
  unsigned *p6 = &arr5[((unsigned)(u3) & 7u)];
  unsigned *p7 = &arr5[7u];
  unsigned *p8 = &arr5[7u];
  unsigned *p9 = &arr5[((unsigned)(u3) & 7u)];
  struct S st10 = { 2224742541u, 3929852116u, 2496609575u };
  struct S st11 = { 1669452725u, 1647024292u, 3190282598u };
  if ((unsigned)(1096567659u) & 1u) {
    cs = csmix(cs, *p9);
    cs = csmix(cs, *p6);
    arr5[((unsigned)(u3) & 7u)] = (unsigned)(((unsigned)((-((unsigned)(((unsigned)((((unsigned)(u4) & 1u) ? (unsigned)(26423076u) : (unsigned)((*p6)))) >= ((unsigned)(((unsigned)((*p6)) - (unsigned)(u4))) ^ cs))) | 0u))) ^ (unsigned)((unsigned)(s2))));
    u3 = (unsigned)(u3) & 0xffffffffu;
    if ((unsigned)(((unsigned)(((unsigned)(u4) * (unsigned)(((unsigned)(((unsigned)(arr5[((unsigned)(u4) & 7u)]) | (unsigned)(1758142783u))) ^ (unsigned)(((unsigned)(1680527174u) ^ (unsigned)((*p6)))))))) << ((unsigned)(1819500812u) & 31u))) & 1u) {
      cs = csmix(cs, *p8);
      cs = csmix(cs, *p7);
    }
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr5[k]);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  cs = csmix(cs, st11.f0);
  cs = csmix(cs, st11.f1);
  cs = csmix(cs, st11.f2);
  cs = csmix(cs, *p6);
  cs = csmix(cs, *p7);
  cs = csmix(cs, *p8);
  cs = csmix(cs, *p9);
  printf("checksum=%08x\n", cs);
  return 0;
}
