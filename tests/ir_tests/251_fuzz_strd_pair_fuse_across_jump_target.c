/* Regression: an O0 STRD/LDRD store-pair fusion peephole (ir/codegen.c) fused
 * two adjacent-spill-slot stores into one STRD across a branch-target NOP.
 *
 * From signed fuzz seed 2987 (verbatim).  tcc -O1/-O2 agreed with the
 * gcc -m32 -funsigned-char oracle (checksum=9df7f66b); tcc -O0 AND -Os
 * HardFaulted (INVSTATE: a `b.w 0` branch to a garbage address).
 *
 * Root cause: the ternary `u5 = cond ? st10.f2 : (s2 << shift)` lowers to two
 * arms that each store their result to the same spill slot, converging at a
 * merge NOP that is the false-arm jump target.  The post-merge assignment
 * (`T55 <- i12`) stores to the adjacent slot, so the ASSIGN-case STRD peephole
 * fused the true-arm store with the post-merge store into a single STRD --
 * skipping the merge NOP between them without noticing it was a branch target.
 * That put both stores on the true-arm path only: the false arm both lost its
 * value and, because the skipped merge NOP never received a code address, its
 * forward jump backpatched to address 0 -> HardFault.
 *
 * Fix: the store/load-pair, MLAL, and block-copy fusion peepholes must not
 * skip a branch-target NOP when searching for a fusion partner (a jump can land
 * between the two, so they cannot share one instruction).
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
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
  char s1 = (char)(1591239122u & 0xff);
  char s2 = (char)(22910600u & 0xff);
  unsigned u3 = 762836346u;
  unsigned u4 = 1526053671u;
  unsigned u5 = 3584281972u;
  unsigned u6 = 2698756343u;
  int si7 = -17883;
  int si8 = 19206;
  int si9 = 20241;
  struct S st10 = { 1626298146u, 1349569430u, 2824948124u };
  struct S st11 = { 1587628206u, 209462030u, 978232572u };

  for (unsigned g13 = 0u; g13 < 3u; g13++) {
    unsigned i12 = g13;
    cs = csmix(cs, i12);
    si9 = (25903) + (si7);
    u5 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) | (unsigned)(i12))) + (unsigned)(3669701946u))) / ((unsigned)((unsigned)(s1)) | 1u))) & 1u) ? (unsigned)(st10.f2) : (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(((unsigned)(st11.f2) + (unsigned)(((unsigned)(i12) % ((unsigned)(st11.f2) | 1u))))) & 31u))))) & 0xffffffffu;
    u5 = (unsigned)(i12) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((si7) < (si9)) ? 1 : 0));
    si9 = (14962) + (si7);
  }
  st11.f2 = (unsigned)(((unsigned)((-((unsigned)(((unsigned)(u5) | (unsigned)((unsigned)(s1)))) | 0u))) >> ((unsigned)(u3) & 31u)));

  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)(si7));
  cs = csmix(cs, (unsigned)(si8));
  cs = csmix(cs, (unsigned)(si9));
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  cs = csmix(cs, st11.f0);
  cs = csmix(cs, st11.f1);
  cs = csmix(cs, st11.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
