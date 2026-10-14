/* ssa:bool_norm and barrel-shift-annotated producers.
 *
 * bool_norm deletes `CMP v,#0; SETIF NE` when v is provably 0 or 1, and it
 * proves that by walking v's definition: a SETIF is 0/1, an OR/AND/XOR of 0/1
 * values is 0/1.  Barrel-shift fusion runs before the SSA engine and folds a
 * single-use shift into the OR's src2 (`orr.w r2, r1, r0, lsl #1`); the shift
 * lives in a side table keyed by orig_index, not in the operand, so the walk
 * saw `SETIF | SETIF` where the value is `SETIF | SETIF << 1` -- 0..3 -- and the
 * `(_Bool)` normalisation vanished.  or_shl / ret_bool are wrong on the plain
 * base; mixed (an int8 re-extension between the two that ext_elim removes)
 * only became reachable with ext_elim. */
#include <stdint.h>
#include <stdio.h>

#define NI __attribute__((noinline))

NI int or_shl(int a, int b, int c)
{
  int v = (int)((unsigned)(a == b) << 1) | (c != 0);
  return (int16_t)((char)(_Bool)v | 0);
}

NI int ret_bool(int a, int b, int c)
{
  int v = (int)((unsigned)(a == b) << 1) | (c != 0);
  return (_Bool)v;
}

NI int xor_shl(int a, int b, int c)
{
  int v = (int)((unsigned)(a < b) << 3) ^ (c > 0);
  return (_Bool)v == 1;
}

NI int and_shr(unsigned a, int b, int c)
{
  /* (a >> 31) is 0/1 only through the shift; the AND of it with a 0/1 is
   * 0/1, so this one must stay correct either way. */
  int v = (int)(a >> 31) & (b != c);
  return (_Bool)v;
}

NI int mixed(int a, int b, int64_t q)
{
  signed char v0 = a;
  int32_t v2 = (int32_t)((uint32_t)(((v0 & (char)b) == v0)) << 1) |
               (int32_t)(((uint32_t)(int8_t)(_Bool)(int32_t)(q >> 32) << 16) >> 16);
  return (int16_t)((char)(_Bool)v2 | 0);
}

/* Minimal ext_elim-reachable form: the int8 round trip of t goes away and the
 * `<< 2` fuses into the ORR (`orr.w r2, r1, r0, lsl #2`). */
NI int b4(int a, int b, int c)
{
  int8_t t = (int8_t)(a < b);
  int v = ((unsigned)(int8_t)t << 2) | (c != 0);
  return (_Bool)v;
}

static const uint32_t W[] = {0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x80000000u, 0xffffffffu};
#define NW 9

int main(void)
{
  unsigned h1 = 0, h2 = 0, h3 = 0, h4 = 0, h5 = 0, h6 = 0;
  int bad = 0;
  for (int i = 0; i < NW; i++)
    for (int j = 0; j < NW; j++)
      for (int k = 0; k < NW; k++)
      {
        int a = (int)W[i], b = (int)W[j], c = (int)W[k];
        int r1 = or_shl(a, b, c), r2 = ret_bool(a, b, c), r3 = xor_shl(a, b, c);
        int r4 = and_shr(W[i], b, c), r5 = mixed(a, b, (int64_t)((uint64_t)W[k] << 32));
        int r6 = b4(a, b, c);
        bad += (r1 & ~1) != 0 || (r2 & ~1) != 0 || (r3 & ~1) != 0 || (r4 & ~1) != 0 || (r5 & ~1) != 0 || (r6 & ~1) != 0;
        h1 = h1 * 3 + r1;
        h2 = h2 * 3 + r2;
        h3 = h3 * 3 + r3;
        h4 = h4 * 3 + r4;
        h5 = h5 * 3 + r5;
        h6 = h6 * 3 + r6;
      }
  printf("or_shl %08x\nret_bool %08x\nxor_shl %08x\nand_shr %08x\nmixed %08x\nb4 %08x\nnon-bool %d\n", h1, h2, h3,
         h4, h5, h6, bad);
  return 0;
}
