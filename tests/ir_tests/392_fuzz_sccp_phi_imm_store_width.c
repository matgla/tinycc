/* Fuzz seed bitfield:372 (-O1; also struct_byval:6988, longlong:6393,
 * signed:8890): sccp's phi-constant fold built the immediate with the PHI's
 * btype — INT16, from the `(unsigned)s1` short arm — and installed it
 * verbatim into every use, including a STORE_INDEXED value operand.  The
 * word store of st6.f1 became strh, leaving the field's upper halfword at
 * its init value.  Fix: sccp_set_instr_operands_imm adopts each use site's
 * btype/signedness (same family as tests 385/390). */
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
struct BF {
  unsigned b0 : 6;
  unsigned b1 : 4;
  unsigned b2 : 2;
  unsigned b3 : 11;
  unsigned b4 : 4;
};
struct BFP {
  unsigned b0 : 8;
  unsigned b1 : 7;
  unsigned b2 : 13;
} __attribute__((packed));
int main(void)
{
  unsigned cs = 0x12345678u;
  short s1 = (short)(1858817297u & 0xffff);
  short s2 = (short)(1279992089u & 0xffff);
  short s3 = (short)(1589268387u & 0xffff);
  unsigned u4 = 1301069303u;
  unsigned u5 = 907381145u;
  struct S st6 = { 4170121569u, 3864744021u, 4191149703u };
  struct BFP bf7 = { 0u, 0u, 0u };
  struct BF bf8 = { 0u, 0u, 0u, 0u, 0u };
  if ((unsigned)(u4) & 1u) {
  }
  st6.f1 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(st6.f0) + (unsigned)(((unsigned)(st6.f0) ^ cs)))) - (unsigned)(1751152940u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(st6.f1) << ((unsigned)(((unsigned)(st6.f2) ^ (unsigned)(st6.f0))) & 31u))) - (unsigned)((((unsigned)((unsigned)(s3)) & 1u) ? (unsigned)(((unsigned)(st6.f2) + (unsigned)(u4))) : (unsigned)(3539785607u))))) : (unsigned)((unsigned)(s1))));
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st6.f0);
  cs = csmix(cs, st6.f1);
  cs = csmix(cs, st6.f2);
  cs = csmix(cs, bf7.b0);
  cs = csmix(cs, bf7.b1);
  cs = csmix(cs, bf7.b2);
  cs = csmix(cs, bf8.b0);
  cs = csmix(cs, bf8.b1);
  cs = csmix(cs, bf8.b2);
  cs = csmix(cs, bf8.b3);
  cs = csmix(cs, bf8.b4);
  printf("checksum=%08x\n", cs);
  return 0;
}
