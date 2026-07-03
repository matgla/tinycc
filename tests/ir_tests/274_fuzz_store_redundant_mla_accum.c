/* Fuzz regression (bitfield seed 17717; O1/O2 miscompile), reduced:
 * store_redundant's read scan (RSE_EVICT_FOR_SRC / RSE_FLUSH_*) covered only
 * src1/src2, so a packed-struct field init store read ONLY through an MLA
 * accumulator deref (`T12 <-- Ta MLA Tb + T3***DEREF***`) looked
 * overwritten-without-read and was NOP'd; the surviving load then read an
 * uninitialized slot.  Fix: run all three evict/flush macros on the MLA
 * accumulator operand too. */
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
} __attribute__((packed));
int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(1599579491u & 0xffff);
  unsigned u7 = 3572735902u;
  unsigned arr8[8] = { 2379383226u, 4114381479u, 2236937157u, 2942316056u, 2878589299u, 4281711162u, 1676002007u, 2341638444u };
  struct S st10 = { 2923808688u, 3183988187u, 573397694u };
  st10.f1 = (unsigned)(((unsigned)(u7) & (unsigned)(((unsigned)(st10.f1) + (unsigned)(((unsigned)((((unsigned)(arr8[((unsigned)(4108781271u) & 7u)]) & 1u) ? (unsigned)(2016220556u) : (unsigned)((unsigned)(s3)))) * (unsigned)((-((unsigned)(arr8[((unsigned)(u7) & 7u)]) | 0u)))))))));
  cs = csmix(cs, st10.f1);
  printf("checksum=%08x\n", cs);
}
