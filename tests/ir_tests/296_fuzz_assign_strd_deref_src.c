/*
 * agg_deep fuzz seeds 52367 / 53515 reduction (O2 HardFault, wrong-code class).
 *
 * Two-level pointer:  unsigned *pa216 = &u7;  unsigned **ppa217 = &pa216;
 * used as both an rvalue (**ppa217) and an lvalue (**ppa217 = ...) under
 * register pressure that spills the intermediate *ppa217 load.
 *
 * The codegen ASSIGN-lowering STRD peephole (ir/codegen.c) fused two adjacent
 * "assign register to adjacent spill slot" ops into one STRD -- but did not
 * check needs_deref on the register source.  A def  T46 <- *ppa217  (a LOAD,
 * carried as an ASSIGN whose REG src has needs_deref set) was thus fused as a
 * plain spill of the *pointer* register (ppa217 = &pa216) rather than the
 * dereferenced value (pa216 = &u7), silently dropping one level of
 * indirection.  The later store  *T46 = x  then wrote through &pa216,
 * corrupting pa216, so the next  **ppa217  read dereferenced a garbage pointer
 * and the program took a precise BusFault (HardFault) at -O2 only.
 *
 * The sibling STORE_INDEXED / STORE STRD peepholes already guarded this with
 * !src1.needs_deref; the fix mirrors that guard on the ASSIGN peephole (both
 * halves).  -O0/-O1/-Os were always correct.
 *
 * Ground truth (tcc -O0 == tcc -O1 == arm-none-eabi-gcc -O2): 5cf3138a.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((-((unsigned)(pb) | 0u))) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)((-((unsigned)(helper1(pb, lr)) | 0u))) | (unsigned)(((unsigned)((~((unsigned)(1008708781u) | 0u))) ^ (unsigned)(((unsigned)(pa) == ((unsigned)(4164424511u) ^ lr))))))) ^ lr;
}
static unsigned helper3(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(3800508290u) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
struct N { unsigned a; unsigned b; };
struct N2 { struct N n; unsigned t; };
int main(void)
{
  unsigned cs = 0x12345678u;
  int s4 = (int)(891898100u & 0xffffffff);
  long s5 = (long)(1532997134u & 0xffffffff);
  unsigned u6 = 3282463992u;
  unsigned u7 = 1771805893u;
  unsigned u8 = 1848722522u;
  unsigned u9 = 454456390u;
  unsigned u10 = 1626658518u;
  unsigned u11 = 1475415131u;
  struct S st12 = { 1196946546u, 657071812u, 3971721373u };
  struct S st13 = { 1292905100u, 2267333376u, 3693812901u };
  struct N2 n214 = { { 1613742539u, 773276573u }, 3554384472u };
  unsigned m215[4][4] = { { 265440588u, 2042073810u, 2721000443u, 2208791278u }, { 2683093u, 2309895808u, 3442626568u, 1865269179u }, { 1964570533u, 3950676023u, 181633839u, 2216349131u }, { 1711725175u, 989902100u, 3044862382u, 837920620u } };
  unsigned *pa216 = &u7;
  unsigned **ppa217 = &pa216;
  { unsigned g19 = 0u;
    while (g19 < 5u) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      for (unsigned g21 = 0u; g21 < 1u; g21++) {
        unsigned i20 = g21;
        cs = csmix(cs, i20);
        cs = csmix(cs, *(&m215[((unsigned)(i20) & 3u)][0] + ((unsigned)(u10) & 3u)));
        cs = csmix(cs, (unsigned)((unsigned)(s5)));
      }
      u9 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(((unsigned)(u9) & (unsigned)(n214.n.b))) << ((unsigned)(((unsigned)(3422966602u) * (unsigned)(3693937913u))) & 31u))) - (unsigned)(1772631384u))) & 1u) ? (unsigned)(n214.t) : (unsigned)((**ppa217)))) & 0xffffffffu;
      **ppa217 = (unsigned)(((unsigned)((((unsigned)(((unsigned)(((unsigned)(m215[((unsigned)(1004642606u) & 3u)][((unsigned)(2521104187u) & 3u)]) + (unsigned)(i18))) < ((unsigned)((**ppa217)) ^ cs))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(3223865410u) <= ((unsigned)(n214.t) ^ cs))) ^ (unsigned)((**ppa217)))) : (unsigned)(((unsigned)((**ppa217)) + (unsigned)(u8))))) % ((unsigned)(((unsigned)(n214.n.b) + (unsigned)((unsigned)(s4)))) | 1u)));
      cs = csmix(cs, **ppa217);
      cs = csmix(cs, *pa216);
      cs = csmix(cs, **ppa217);
      cs = csmix(cs, *pa216);
      i18 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((**ppa217)) + (unsigned)((unsigned)(s5)))) * (unsigned)(((unsigned)(st12.f1) | (unsigned)(st13.f2))))) - (unsigned)(((unsigned)(((unsigned)(188839645u) << ((unsigned)(m215[((unsigned)(u7) & 3u)][((unsigned)(u9) & 3u)]) & 31u))) - (unsigned)(((unsigned)(u9) / ((unsigned)(n214.t) | 1u))))))) / ((unsigned)(((unsigned)(st12.f1) % ((unsigned)(((unsigned)(((unsigned)(1313794045u) % ((unsigned)((unsigned)(s4)) | 1u))) ^ (unsigned)(((unsigned)(m215[((unsigned)(1856198353u) & 3u)][((unsigned)(i18) & 3u)]) & (unsigned)(st12.f2))))) | 1u))) | 1u))) & 0xffffffffu;
      g19++;
    }
  }
  cs = csmix(cs, (unsigned)(helper2(n214.n.b, ((unsigned)(u7) ^ (unsigned)(((unsigned)(n214.n.b) >> ((unsigned)((~((unsigned)(u11) | 0u))) & 31u)))))));
  cs = csmix(cs, *(&m215[((unsigned)(1974416223u) & 3u)][0] + ((unsigned)(u6) & 3u)));
  if ((unsigned)((-((unsigned)(u8) | 0u))) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((-((unsigned)(3274189075u) | 0u))) / ((unsigned)(((unsigned)(m215[((unsigned)(u9) & 3u)][((unsigned)(u6) & 3u)]) << ((unsigned)(u8) & 31u))) | 1u))) / ((unsigned)(((unsigned)(((unsigned)(u10) + (unsigned)(u8))) >> ((unsigned)(((unsigned)(st12.f0) | (unsigned)(((unsigned)(st12.f0) ^ cs)))) & 31u))) | 1u))) | (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s5)) % ((unsigned)(u8) | 1u))) * (unsigned)(n214.n.b))) | (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) | (unsigned)(u6))) ^ (unsigned)(m215[((unsigned)(3684409688u) & 3u)][((unsigned)(u7) & 3u)]))))))));
    { unsigned g23 = 0u;
      while (g23 < 5u) {
        unsigned i22 = g23;
        cs = csmix(cs, i22);
        cs = csmix(cs, *(&m215[((unsigned)(i22) & 3u)][0] + ((unsigned)(u9) & 3u)));
        cs = csmix(cs, *(&m215[((unsigned)(u10) & 3u)][0] + ((unsigned)(2413244270u) & 3u)));
      }
    }
    { unsigned g25 = 0u;
      while (g25 < 12u) {
        unsigned i24 = g25;
        cs = csmix(cs, i24);
        cs = csmix(cs, *(&m215[((unsigned)(i24) & 3u)][0] + ((unsigned)(3928986626u) & 3u)));
        cs = csmix(cs, *(&m215[((unsigned)(3994467457u) & 3u)][0] + ((unsigned)(4052005135u) & 3u)));
        cs = csmix(cs, *(&m215[((unsigned)(354394584u) & 3u)][0] + ((unsigned)(i24) & 3u)));
        cs = csmix(cs, **ppa217);
        cs = csmix(cs, *pa216);
        cs = csmix(cs, (unsigned)((**ppa217)));
      }
    }
  }
  cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1719122532u) * (unsigned)((unsigned)(s4)))) ^ (unsigned)(m215[((unsigned)(u8) & 3u)][((unsigned)(1280512280u) & 3u)]))));
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, u10);
  cs = csmix(cs, u11);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, helper3(38177487u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  cs = csmix(cs, st12.f0);
  cs = csmix(cs, st12.f1);
  cs = csmix(cs, st12.f2);
  cs = csmix(cs, st13.f0);
  cs = csmix(cs, st13.f1);
  cs = csmix(cs, st13.f2);
  cs = csmix(cs, n214.n.a);
  cs = csmix(cs, n214.n.b);
  cs = csmix(cs, n214.t);
  for (unsigned ii = 0u; ii < 4u; ii++) for (unsigned jj = 0u; jj < 4u; jj++) cs = csmix(cs, m215[ii][jj]);
  cs = csmix(cs, **ppa217);
  cs = csmix(cs, *pa216);
  printf("checksum=%08x\n", cs);
  return 0;
}
