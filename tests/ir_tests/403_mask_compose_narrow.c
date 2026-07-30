/* Guard: mask-composition narrowing in ssa:narrow (narrow_and).
 *
 * A bitfield read narrower than its storage unit emits "truncate to the
 * container width, then mask to the field width" — e.g. `UBFX #0,#16` (the
 * `unsigned short` truncation) feeding `& 0x7ff` (the 11-bit field).  The inner
 * mask is backward-redundant: no forward known-bits pass can drop it, but the
 * two masks compose to `v & (m1 & m2)`, and a UBFX(v,0,w) counts as an AND.
 * When the composed mask is a contiguous low-bits mask the fold emits the
 * canonical UBFX form, so the downstream bitfield store->load forward (the fn3
 * shape) still matches.
 *
 * These check the value is preserved for: the truncate-then-field read (fnA/
 * fnB), the store-then-read fn3 shape, a nested plain-AND compose, and an
 * explicit unsigned-short truncation feeding a narrower mask.
 */
#include <stdio.h>

struct __attribute__((packed)) A { unsigned short i : 1, l : 1, j : 3, k : 11; };
struct __attribute__((packed)) B { unsigned int i : 6, j : 2, k : 8, l : 16; };
static struct A sA;
static struct B sB;

__attribute__((noinline)) unsigned int fnA(unsigned int x) { struct A y = sA; y.k += x; return y.k; }
__attribute__((noinline)) unsigned int fnB(unsigned int x) { struct B y = sB; y.k += x; return y.k; }
__attribute__((noinline)) unsigned int fn3A(unsigned int x) { sA.k += x; return sA.k; }
__attribute__((noinline)) unsigned int nested(unsigned int x) { return (x & 0x3ff) & 0xff; }
__attribute__((noinline)) unsigned int trunc_mask(unsigned int x) { unsigned short s = x; return s & 0x7ff; }

int main(void)
{
  /* Each result captured in a defined order: fn3A mutates the global sA that
   * fnA reads, so they must not share a printf argument list (unspecified
   * evaluation order). */
  unsigned int a, b, c, n, t;
  sA.k = 1234;
  sB.k = 200;
  a = fnA(5);
  b = fnB(60000);
  n = nested(0x2ab);
  t = trunc_mask(0xabcd);
  c = fn3A(7);
  printf("fnA=%u fnB=%u fn3A=%u nested=%u trunc=%u\n", a, b, c, n, t);
  printf("after fn3A sA.k=%u\n", sA.k);
  return 0;
}
