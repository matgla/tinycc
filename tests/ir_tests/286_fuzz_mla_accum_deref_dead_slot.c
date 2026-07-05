/*
 * struct_byval / combo fuzz seed 26687 reduction (O1/O2 wrong-code):
 * dead_local_slot_elim's tameness-classification loop scanned only
 * dest/src1/src2, never the MLA accumulator (pool[base+3]).  A by-value
 * struct field `p.a` was read through `T <- Addr[StackLoc[p.a-home]];
 * MLA x*0 + T***DEREF***`.  sl_forward forwarded p.a's *direct* reads to the
 * parameter register and dead_local_slot then deleted the home store
 * `StackLoc[p.a] <- P1` -- because the only surviving read was the MLA
 * accumulator deref, which the tameness loop never saw.  The function's
 * `r.b` write is a STORE_INDEXED, so dls_precise_ok was false and the
 * mirrored precise-read path in the live-collection loop was gated off too,
 * leaving the slot with no recorded read.  The MLA then read an
 * uninitialized stack slot.
 * Fixed by extending the tameness loop to k==3 (the MLA accumulator).
 * Ground truth (tcc -O0 == gcc -O2): checksum=59222f5d
 * (buggy tcc -O1 emitted 8384424e).
 */
#include <stdio.h>

struct SB8 { unsigned a; unsigned b; };
struct SB5 { unsigned a; unsigned char b; };

static struct SB5 sbh4(struct SB8 p, unsigned x)
{
  struct SB5 r = { x ^ (p.a * 3u), (unsigned char)(p.a & 0xffu) };
  /* Denominator `p.a + x*(x^x)` fuses into `MLA x,(x^x),p.a`, folding the
   * p.a field read into the accumulator's Addr[StackLoc]-deref; the r.b
   * write below is a STORE_INDEXED. */
  r.b = (unsigned char)(((p.b | 682602259u) / ((p.a + x * (x ^ x)) | 1u)) & 0xffu);
  return r;
}

int main(void)
{
  struct SB8 a = { 3908944757u, 287684389u };
  struct SB5 r = sbh4(a, 3822806274u);
  unsigned cs = r.a ^ (r.b * 2654435761u);
  printf("checksum=%08x\n", cs);
  return 0;
}
