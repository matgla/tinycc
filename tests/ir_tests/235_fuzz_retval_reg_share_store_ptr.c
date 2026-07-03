/* Regression: the linear-scan "return-block register sharing" optimization
 * clobbered a store's base-pointer register, producing `str r0, [r0]` (a store
 * of a value through itself) -> a wild write -> HardFault.
 *
 * Reduced from differential-fuzz gen_c.py --profile struct_byval seed=62
 * (tcc -O1/-O2 HardFault; -O0/-Os and arm-none-eabi-gcc -O2 == checksum=c2dfe9e8).
 *
 * Pass: ir/regalloc.c  (return-block register sharing in the linear-scan
 * allocator; gated by -fstore-load-fwd, which is what forwards the stored value
 * straight into RETURNVALUE and thus makes it want r0).
 *
 * Bug: sbh3 returns a single-field struct SB4 in r0.  Its body computes the
 * final value T20, stores it through the pointer T10 = &r (the return slot,
 * which is address-taken so the store survives dead-store-elim) and then
 * returns T20.  store-load-fwd rewrites `load r; return r` into `return T20`,
 * so T20 prefers r0 (the return register).  r0 is still held by T10, so the
 * allocator's return-block sharing kicked in: its conflict scan asked "is the
 * partner (T10) read anywhere in [def(T20), return]?" but only looked at src1/
 * src2 operands -- it never checked that a STORE *reads its dest operand* (the
 * base pointer).  It concluded T10 was dead, shared r0 between T10 and T20, and
 * emitted `str r0, [r0]`: the value overwrote the address before the store.
 *
 * Fix: the conflict scan now uses ra_instr_touches_vreg(), which counts the
 * STORE-class dest (and the MLA accumulator) as a read -- so a partner used as
 * a store base within the range blocks the share, exactly as -fno-store-load-fwd
 * already did by not forwarding into the return.
 *
 * Ground truth arm-none-eabi-gcc -O2 == tcc -O0 == checksum=c2dfe9e8.
 * Was: -O1/-O2 HardFault.
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

struct SB4 { unsigned a; };
struct SB8 { unsigned a; unsigned b; };

static struct SB4 sbh3(struct SB8 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) };
  r.a = (unsigned)(p.a + (~((((x & 1u) ? 3512256450u : 2541753822u)) | 0u)));
  r.a = (unsigned)(((-((1459015484u) | 0u)) & 1u)
                       ? ((p.a - 1817535604u) / (1668412597u | 1u))
                       : (((1533964464u > (2246018482u ^ x))
                           + ((x & 1u) ? p.b : 2443037461u))));
  return r;
}

int main(void)
{
  unsigned cs = 0x12345678u;
  struct SB8 a = { 0x11u, 0x22u };
  struct SB4 t = sbh3(a, cs);
  cs = csmix(cs, t.a);
  printf("checksum=%08x\n", cs);
  return 0;
}
