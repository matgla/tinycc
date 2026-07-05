/*
 * volatile fuzz seed 36818 reduction (O2 wrong-code):
 * post-RA move coalescing put the inner loop's in-place `u8 = ~u8` XOR temp
 * onto the same register as its source (a deliberate two-address overlap),
 * then a second coalesce moved the source vreg away and blindly cleared the
 * shared register's live_regs_by_instruction bits over its whole range,
 * orphaning the XOR temp's claim. The phase-3 scratch-conflict fixup trusted
 * the bitmap and moved the outer loop counter onto that register, so the
 * inner loop clobbered the counter and the outer loop ran once instead of 4x.
 * Bitmap clears now keep bits set while another claimant interval is live.
 * Ground truth (tcc -O0 == gcc -O2 on the original seed): checksum=d1ba35a4.
 * This reduced form's ground truth is checksum=53998b73.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(pb) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(helper1((((unsigned)(lr) & 1u) ? (unsigned)(pb) : (unsigned)(((unsigned)(lr) ^ (unsigned)(((unsigned)(lr) ^ lr))))), ((unsigned)((-((unsigned)(1323860999u) | 0u))) | (unsigned)(3396356937u)))) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(lr) ^ lr;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  long s4 = (long)(1238971132u & 0xffffffff);
  short s5 = (short)(1558347237u & 0xffff);
  long s6 = (long)(1491118456u & 0xffffffff);
  unsigned u7 = 171217551u;
  unsigned u8 = 3819010045u;
  unsigned u9 = 3480379661u;
  unsigned arr10[8] = { 505270059u, 3528081990u, 4282465689u, 3277507143u, 973903530u, 3262618199u, 1981257833u, 2929150763u };
  volatile unsigned vv11 = 1613047895u;
  volatile unsigned vv12 = 2044628481u;
  volatile unsigned vv13 = 2582252458u;
  cs = csmix(cs, (unsigned)(((unsigned)(1789995263u) - (unsigned)(1084081821u))));
  arr10[((unsigned)(u8) & 7u)] = (unsigned)(u7);
  { unsigned g15 = 0u;
    while (g15 < 4u) {
      unsigned i14 = g15;
      cs = csmix(cs, i14);
      vv11 = (unsigned)(u8);
      { unsigned g17 = 0u;
        while (g17 < 12u) {
          unsigned i16 = g17;
          cs = csmix(cs, i16);
          u8 = (unsigned)((~((unsigned)(u8) | 0u))) & 0xffffffffu;
          g17++;
        }
      }
      arr10[((unsigned)(u9) & 7u)] = (unsigned)(((unsigned)(u8) % ((unsigned)((unsigned)(s4)) | 1u)));
      i14 = (unsigned)(657895072u) & 0xffffffffu;
      g15++;
    }
  }
  cs = csmix(cs, (unsigned)((-((unsigned)(u7) | 0u))));
  vv11 = (unsigned)(((unsigned)(helper2(u7, u9)) << ((unsigned)(((unsigned)(((unsigned)(u8) + (unsigned)(((unsigned)(4264903326u) << ((unsigned)(3563225252u) & 31u))))) & (unsigned)((unsigned)(s4)))) & 31u)));
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, vv11);
  cs = csmix(cs, vv12);
  cs = csmix(cs, vv13);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, (unsigned)s6);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
