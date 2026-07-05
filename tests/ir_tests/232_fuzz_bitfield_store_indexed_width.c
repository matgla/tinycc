/* Fuzz bitfield seed 30: a packed-bitfield byte store must not widen to a
 * word when its value operand's btype is forwarded/widened during opt.
 *
 * `bf13.b2 = arr12[...] & 7` stores a 3-bit field that straddles two bytes of
 * the packed struct.  The high-bit part lowers to a narrow (INT8) store to the
 * byte just before `arr12`.  A plain STORE takes its width from the dest
 * (lvalue) btype, but the value-forwarding that collapses the field's
 * read-modify-write (`(w & ~field) | x`) replaces the store's value operand
 * with a wider (INT32) temp.  When that store is later turned into a
 * STORE_INDEXED — which takes its width from the VALUE operand's btype — the
 * byte store became a word store, writing 4 bytes and clobbering the low 3
 * bytes of arr12[0] (cfd68b64 -> cf000000).
 *
 * Fix: carry the narrow access width onto the store value before any
 * plain-STORE -> STORE_INDEXED conversion, and never let a value rewrite widen
 * an existing STORE_INDEXED / STORE_POSTINC value operand.
 *
 * Wrong (O1/O2/before fix): checksum=8e992026  (arr12[0] low bytes zeroed)
 * Correct (O0 / fixed):     checksum=4f04b3a6
 */
#include <stdio.h>

static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}

static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  if (lr & 1u) lr += (3562009314u % (pb | 1u)) & (3003828842u | 3305557305u);
  return (1450206822u % (2085853371u | 1u)) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = pb;
  return ((lr == (pb ^ lr)) + (pb & 406485683u)) ^ lr;
}

struct BFP {
  unsigned b0 : 1;
  unsigned b1 : 13;
  unsigned b2 : 3;
  unsigned b3 : 6;
} __attribute__((packed));

int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(817958909u & 0xff);
  char s4 = (char)(642790074u & 0xff);
  short s5 = (short)(828167721u & 0xffff);
  unsigned u6 = 590902416u;
  unsigned u7 = 1902681567u;
  unsigned u8 = 955975220u;
  unsigned u9 = 615622931u;
  unsigned u10 = 707108199u;
  unsigned u11 = 1782458143u;
  unsigned arr12[8] = { 3486944100u, 3172852712u, 3033945768u, 2062165485u,
                        4152962953u, 3338905966u, 2389143983u, 3849955225u };
  struct BFP bf13 = { 0u, 0u, 0u, 0u };

  cs = csmix(cs, (unsigned)s4 | 1505637481u);
  bf13.b2 = arr12[u7 & 7u] & ((1u << 3) - 1u);
  u7 = (unsigned)s3;
  u6 = (unsigned)s4 ^ ((1692237310u & 1u) ? (unsigned)(-(unsigned)s5) : arr12[3449833019u & 7u]);
  if (arr12[u7 & 7u] & 1u) {
    if (1489267915u >= ((((unsigned)s5 - 303650426u)) ^ cs)) {
    }
  } else {
    bf13.b3 = u8 & ((1u << 6) - 1u);
    u7 = u11;
    for (unsigned g17 = 0u; g17 < 7u; g17++) {
      cs = csmix(cs, g17);
      cs = csmix(cs, 3073859022u < ((u7 - ((arr12[3083061847u & 7u] / (2863261557u | 1u)) & ((unsigned)s5 <= (u8 ^ cs)))) ^ cs));
      arr12[u11 & 7u] = (u11 / (u10 | 1u)) >> ((u9 + (((unsigned)s5 | (unsigned)s3) | u9)) & 31u);
    }
    u8 = 1207505990u;
  }

  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr12[k]);
  cs = csmix(cs, bf13.b0);
  cs = csmix(cs, bf13.b1);
  cs = csmix(cs, bf13.b2);
  cs = csmix(cs, bf13.b3);
  printf("checksum=%08x\n", cs);
  return 0;
}
