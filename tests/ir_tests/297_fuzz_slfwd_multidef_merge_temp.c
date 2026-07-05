/* ptr fuzz seed 72674 (O2): store-load forwarding (ir/opt_memory.c
 * tcc_ir_opt_sl_forward) mis-forwarded a ternary result.  A merge temp T29
 * (defined on both arms of a `?:` diamond) had its else-arm def forwarded from
 * a LOAD; that forward re-marked fwd_tmp_valid[T29]=1 without the pre-scan's
 * multi-def check, so the merge store `V16 <- T29` resolved its value through
 * the stale else-arm constant and the following `V17 <- V16` load forwarded the
 * wrong arm.  Fixed by rejecting multiply-defined temps at the fwd_tmp_val
 * consume sites. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(pb);
  return (unsigned)(3370544418u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  char s3 = (char)(460672706u & 0xff);
  unsigned u8 = 2442128116u;
  unsigned u9 = 3154266738u;
  unsigned arr10[8] = { 2692430149u, 3838425689u, 3794898626u, 1606399006u, 2768957426u, 561834519u, 3335066530u, 2447764398u };
  unsigned arr11[8] = { 1967696042u, 2182923645u, 2092935298u, 2256754076u, 3330512201u, 503243934u, 1984351027u, 3305754278u };
  unsigned *p12 = &arr10[7u];
  unsigned *p13 = &u8;
  unsigned *p14 = &arr11[((unsigned)(u9) & 7u)];
  struct S st15 = { 3493538719u, 416352386u, 3281875180u };
  struct S st16 = { 4143340285u, 3375350326u, 1814528950u };
  unsigned cond = ((unsigned)(((unsigned)(2308804676u) ^ (unsigned)(st16.f2)) != ((unsigned)(helper1((*p12), arr11[((unsigned)(2040205554u) & 7u)])) ^ cs))) & 1u;
  unsigned c = cond ? (-(((*p14) | (unsigned)(st15.f1)))) : (helper1(((unsigned)((*p13)) ^ (unsigned)((unsigned)(s3))), (*p14)));
  cs = csmix(cs, helper1(0u, c));
  printf("checksum=%08x\n", cs);
  return 0;
}
