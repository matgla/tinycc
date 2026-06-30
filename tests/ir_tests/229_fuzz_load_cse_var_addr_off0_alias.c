/* Regression for differential-fuzz ptr seeds 67/90/157/165/292/405/560/708/967/968:
 * wrong-code at -O2 (all ten 0-1000 ptr divergences shared this root cause).
 *
 * Root cause: ssa_opt_resolve_lea_stackloc (ir/opt/ssa_opt.c), used by the
 * SSA load-CSE / stack store-load forwarder (ssa:load_cse) to map a pointer
 * TEMP back to a concrete stack offset.  For a LEA whose source is the address
 * of a scalar local (`T <- &u2`), the source operand is encoded as a VAR spill
 * reference: tag == STACKOFF, is_local == 1, but with a NON-zero vreg and a
 * PLACEHOLDER offset of 0 (scalar locals have no FP-relative slot at SSA time).
 * tccir_operand.h documents that only operands with vreg_type == 0 are real
 * stack slots; the resolver ignored that and returned the bare offset, so every
 * distinct address-taken local (`&u2`, `&u3`, ...) collapsed to offset 0.
 * load_cse then forwarded a constant stored through one pointer into a load
 * through an unrelated one: `*p8 = X` (p8=&u2) was forwarded into every `*p7`
 * read (p7=&u3), and DCE deleted &u3 and u3's initializer entirely.
 *
 * Here p7=&u3, p8=&u2; the store `*p8 = ~2998950496 + u5` must NOT be visible to
 * the `*p7` reads.  Buggy -O2 folded all `*p7` reads to the `*p8` store value.
 *
 * Fix: resolve a LEA/ASSIGN/STORE address source to a stack offset only when it
 * is a real slot (irop_get_vreg(src) < 0, i.e. vreg_type == 0).  VAR/PARAM
 * spill encodings bail to INT_MIN so forwarding falls back to ptr-vreg identity
 * (tvstore), which correctly keeps &u2 and &u3 distinct.
 * (-fno-const-prop or TCC_DISABLE_PASS=ssa:load_cse also avoid it; the real
 * pass is ssa:load_cse via ssa_opt_resolve_lea_stackloc.)
 */
#include <stdio.h>
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
  long s1 = (long)(2008486473u & 0xffffffff);
  unsigned u2 = 2561328645u;
  unsigned u3 = 3819416746u;
  unsigned u4 = 2765095536u;
  unsigned u5 = 2857882849u;
  unsigned u6 = 1236860582u;
  unsigned *p7 = &u3;
  unsigned *p8 = &u2;
  struct S st9 = { 1323792975u, 54611170u, 4161741471u };
  cs = csmix(cs, (unsigned)((-((unsigned)((((unsigned)((unsigned)(s1)) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)(u3))) | 0u))));
  cs = csmix(cs, (unsigned)(105967105u));
  *p8 = (unsigned)(((unsigned)((~((unsigned)(2998950496u) | 0u))) + (unsigned)(u5)));
  cs = csmix(cs, *p7);
  cs = csmix(cs, *p8);
  cs = csmix(cs, *p7);
  cs = csmix(cs, *p8);
  cs = csmix(cs, *p8);
  cs = csmix(cs, *p7);
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, *p7);
  cs = csmix(cs, *p8);
  printf("checksum=%08x\n", cs);
  return 0;
}
