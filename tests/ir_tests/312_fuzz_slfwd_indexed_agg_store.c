/* Regression for fuzz agg_deep seed 173727: indexed-memory and store-load
 * forwarding must invalidate aggregate entries after runtime indexed stores. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
};
struct N { unsigned a; unsigned b; };
struct N2 { struct N n; unsigned t; };
int main(void)
{
  unsigned cs = 0x12345678u;
  long s1 = (long)(1666243791u & 0xffffffff);
  unsigned u2 = 3485064194u;
  unsigned u3 = 1391043975u;
  unsigned u4 = 565336154u;
  unsigned u5 = 36885620u;
  unsigned u6 = 3365780579u;
  unsigned u7 = 1692965204u;
  unsigned arr8[8] = { 856909510u, 1565767713u, 1431295779u, 1385125445u, 4052774777u, 1380605663u, 3404504237u, 4250872572u };
  struct N2 n29 = { { 503285213u, 3640400246u }, 2880659209u };
  unsigned m210[4][4] = { { 1033210885u, 3527828754u, 2384987994u, 1472404403u }, { 540004466u, 3141008226u, 977583547u, 3129953794u }, { 3344023061u, 843845321u, 412367330u, 1942570189u }, { 140268806u, 2628543977u, 2329623267u, 1845303199u } };
  unsigned *pa211 = &u3;
  unsigned **ppa212 = &pa211;
  u6 = (unsigned)(((unsigned)(m210[((unsigned)(u5) & 3u)][((unsigned)(u2) & 3u)]) | (unsigned)(400963912u))) & 0xffffffffu;
  m210[((unsigned)(1394546781u) & 3u)][((unsigned)(u6) & 3u)] = (unsigned)(((unsigned)((**ppa212)) * (unsigned)(((unsigned)(m210[((unsigned)(u3) & 3u)][((unsigned)(u2) & 3u)]) ^ (unsigned)(u2)))));
  cs = csmix(cs, *(&m210[((unsigned)(1394546781u) & 3u)][0] + ((unsigned)(u6) & 3u)));
  cs = csmix(cs, **ppa212);
  cs = csmix(cs, *pa211);
  cs = csmix(cs, u2);
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, n29.n.a);
  cs = csmix(cs, n29.n.b);
  cs = csmix(cs, n29.t);
  for (unsigned ii = 0u; ii < 4u; ii++) for (unsigned jj = 0u; jj < 4u; jj++) cs = csmix(cs, m210[ii][jj]);
  cs = csmix(cs, **ppa212);
  cs = csmix(cs, *pa211);
  printf("checksum=%08x\n", cs);
  return 0;
}
