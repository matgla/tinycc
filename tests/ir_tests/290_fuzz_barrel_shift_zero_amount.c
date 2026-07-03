/* Fuzz regression: varargs seed 36881 (O1/O2), root cause = late barrel-shift
 * fusion (ir/opt_fusion.c tcc_ir_barrel_shift_fusion).
 *
 * const-prop folds `(unsigned)s3 & 31` to the constant 0 (s3 == -26496, so the
 * low 5 bits are 0), leaving `T = u11 SHR #0` (identity, T == u11) feeding
 * `u11 | ...`.  Barrel-shift fusion folded that shift into the consuming OR as
 * `orr Rd, Rn, Rm, lsr #0`.  On ARM the barrel shifter encodes an immediate
 * field of 0 for LSR/ASR/ROR as shift-by-32 (RRX for ROR), NOT the shift-by-0
 * the IR means, so `u11 lsr #0` became `u11 lsr #32` == 0 and `u11 | u8`
 * collapsed to `u8`, then `u8 % u8 == 0` zeroed the whole product.
 *
 * Fix: don't fuse a zero-amount right shift/rotate (only LSL #0 is a true
 * no-op barrel operand); leave the standalone `SHR #0` for the backend
 * shift-by-0 identity fold to lower as a plain MOV.
 *
 * Expected checksum is the gcc -m32 -funsigned-char / tcc -O0 oracle value.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (~2570290591u) ^ lr;
}
static unsigned vsum(unsigned n, ...)
{
  return 0u;
}
struct S {
  unsigned f0, f1, f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  unsigned u7 = 2035119998u, u8 = 3232245693u, u9 = 2584461659u, u11 = 762638074u;
  short s3 = (short)(1945802880u & 0xffff);
  unsigned arr12[8] = { 2857828080u, 1169030693u, 420301827u, 1864451046u,
                        3280578421u, 1012603370u, 1124098830u, 1546474092u };
  struct S st13 = { 2489298879u, 3180274513u, 1780191234u };
  for (unsigned g = 0u; g < 11u; g++) {
    cs = csmix(cs, (1593724639u ^ st13.f0) *
                       (u8 % ((((u11 >> ((unsigned)s3 & 31u)) | u8) | 1u))));
    cs = csmix(
        cs,
        vsum(7u, 0, 0,
             (int)(((unsigned)(u11)
                    << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(194289888u) ^
                                                             (unsigned)(arr12[((unsigned)(u9) & 7u)]))) <<
                                                ((unsigned)(helper2((unsigned)(s3),
                                                                    arr12[((unsigned)(2659937619u) & 7u)])) &
                                                 31u))) |
                                    (unsigned)(((unsigned)(2655375284u) ^
                                                (unsigned)(((unsigned)(1021384105u) >>
                                                            ((unsigned)(u7) & 31u))))))) &
                        31u))),
             0, 0, 0, 0));
  }
  printf("checksum=%08x\n", cs);
  return 0;
}
