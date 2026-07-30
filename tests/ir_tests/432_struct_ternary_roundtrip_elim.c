/* Guard: struct_copy_roundtrip_elim must not resolve a copy's address temp
 * through the textually-latest def when the temp has defs on BOTH arms of a
 * branch.
 *
 * A struct-valued ternary
 *
 *     Op chosen = cond ? getA(i) : getB(i);
 *
 * lowers to one sret slot per arm plus a phi'd address temp
 * (T2 = &slotA on one path, T2 = &slotB on the other) feeding
 * `memmove(&chosen, T2, 9); memmove(&var, &chosen, 9)`.
 * scre_resolve_slot_addr walked backward to the LATEST def only (T2 = &slotA),
 * concluded the pair was an identity roundtrip on slotA, and deleted both
 * copies — so the cond==0 result was never stored.  In the on-device tcc this
 * corrupted ssa_rewrite_flag_consumer's SELECT rewrite (`chosen = result ?
 * get_src1() : get_src2()`), producing ASSIGNs of garbage operands whenever a
 * TEST_ZERO-folded SELECT picked the false arm.
 */

#include <stdio.h>

typedef struct __attribute__((packed)) Op { unsigned a, b; unsigned char c; } Op;

Op ga = {1, 2, 3};
Op gb = {40, 50, 60};

Op getA(int i) { Op r = ga; r.a += (unsigned)i; return r; }
Op getB(int i) { Op r = gb; r.a += (unsigned)i; return r; }

int use(Op x) { return (int)(x.a + x.b + x.c); }

volatile int vc, vi;

int pick(int c, int i)
{
  vc = c;
  vi = i;
  Op chosen = vc ? getA(vi) : getB(vi);
  return use(chosen);
}

int main(void)
{
  printf("p1=%d\n", pick(1, 10));
  printf("p0=%d\n", pick(0, 7));
  return 0;
}
