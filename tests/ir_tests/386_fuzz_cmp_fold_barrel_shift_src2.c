/* Fuzz seed signed:840 (-O1): the (int)(short) narrowing compare
 * `si9 > (int)(short)u8` fuses into `CMP si9, (T1 ASR #16)` via a barrel-shift
 * annotation on the CMP's src2.  ssa_fold_cmp_jumpif then resolved both
 * operands to constants and evaluated eval_cond on the RAW src2 value —
 * explicitly dropping the annotation ("so the reused NOP/JUMP slot can't
 * inherit it") instead of applying the fused ASR, so the SELECT folded to the
 * wrong arm.  Fix: apply the annotation to v2 in the constant path; bail on
 * the reflexive/bitfield/expr-equal/stack-addr proof paths it invalidates. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S { unsigned f0; unsigned f1; unsigned f2; };
int main(void)
{
  unsigned cs = 0x12345678u;
  int s4 = (int)73543519u;
  unsigned u7 = 2024167460u;
  unsigned u8 = 1860906774u;
  int si9 = 20931;
  int si10 = 23667;
  struct S st11 = { 3128523538u, 4094333093u, 3337492467u };
  if (1) {
    cs = csmix(cs, (unsigned)((si9 > ((int)(short)(u8))) ? 1 : 0));
    { unsigned g15 = 0u;
      while (g15 < 8u) {
        unsigned i14 = g15;
        cs = csmix(cs, i14);
        u8 = u7 | (((i14 | st11.f0) * st11.f2) + u8);
        g15++;
      }
    }
  }
  si9 = si10 >> ((unsigned)s4 & 31u);
  cs = csmix(cs, u8);
  cs = csmix(cs, (unsigned)si9);
  printf("checksum=%08x\n", cs);
  return 0;
}
