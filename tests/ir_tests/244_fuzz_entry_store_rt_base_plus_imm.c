/* Regression: entry-store propagation forwarded a 2-D stack array element's
 * initializer across a store whose address was a RUNTIME array base plus a
 * *constant* column displacement — so the store invalidated nothing and a later
 * constant-offset read of the same cell saw the stale .rodata initializer.
 *
 * From agg_deep fuzz seed 781 (reduced).  tcc -O0 agreed with gcc; tcc -O1/-O2
 * diverged — an O1/O2 store-load-forwarding miscompile.
 *
 * Root cause: `m215[u7 & 3][2] = ...` with u7 a runtime value lowers to a plain
 * STORE through `T44 = T43 + #8`, where `T43 = &m215 + ((u7 & 3) << 4)` is a
 * runtime array base (recorded in rt_base, NOT lea_map).  In
 * tcc_ir_opt_entry_store_prop (ir/opt_memory.c) the `TEMP = TEMP + imm` case of
 * the LEA/rt-base tracker only propagated lea_map (constant bases); it dropped
 * rt_base.  So T44 carried no base, Phase 2.6's runtime-store invalidation could
 * not see that the store hits m215, and the entry BLOCK_COPY initializer of
 * m215[2][2] was forwarded past the loop store into the later `m215[u9&3][u9&3]`
 * (== m215[2][2]) read.
 *
 * Fix: `TEMP = <rt_base pointer> + const` (and the VAR analogue) now carries the
 * runtime array base forward, so the store invalidates the array's initializers.
 *
 * At runtime u7 & 3 == 2 and u9 & 3 == 2, so the loop store and the read-back
 * touch the same cell m215[2][2].
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
  return (unsigned)((((unsigned)(((unsigned)(pb) * (unsigned)(((unsigned)(4204229120u) * (unsigned)(pb))))) & 1u) ? (unsigned)(720568408u) : (unsigned)(lr))) ^ lr;
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
  short s2 = (short)(192199693u & 0xffff);
  char s3 = (char)(1229364503u & 0xff);
  long s4 = (long)(740485389u & 0xffffffff);
  unsigned u5 = 3570016566u;
  unsigned u6 = 3835912957u;
  unsigned u7 = 4271514640u;
  unsigned u8 = 1691095234u;
  unsigned u9 = 2392025914u;
  unsigned arr10[8] = { 3240706797u, 2801717459u, 1633265650u, 1303071792u, 366568635u, 1958337308u, 3898993183u, 1287073424u };
  unsigned arr11[8] = { 2883301829u, 3582224331u, 777208215u, 385008802u, 2520629266u, 2162315569u, 3368932533u, 1520301711u };
  struct S st12 = { 2167290569u, 8094703u, 2264247776u };
  struct S st13 = { 3123288377u, 2767151351u, 4144517752u };
  struct N2 n214 = { { 626995679u, 1430081621u }, 118726769u };
  unsigned m215[4][4] = { { 843299079u, 2189604987u, 1167711615u, 2068332441u }, { 1006934090u, 4220822380u, 3489195633u, 752035597u }, { 259359580u, 1822242713u, 4105124166u, 1066003974u }, { 1130690806u, 3526643872u, 1524399196u, 4130523655u } };
  unsigned *pa216 = &u8;
  unsigned **ppa217 = &pa216;
  cs = csmix(cs, **ppa217);
  cs = csmix(cs, *pa216);
  { unsigned g19 = 0u;
    while (g19 < 10u) {
      unsigned i18 = g19;
      cs = csmix(cs, i18);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(u9) & 1u) ? (unsigned)(arr10[((unsigned)(u8) & 7u)]) : (unsigned)((unsigned)(s2)))) ^ (unsigned)((-((unsigned)((**ppa217)) | 0u))))) ^ (unsigned)(arr10[((unsigned)(i18) & 7u)]))) % ((unsigned)(u7) | 1u))));
      u7 = (unsigned)((~((unsigned)(((unsigned)(n214.n.a) / ((unsigned)(((unsigned)(n214.t) << ((unsigned)(((unsigned)(n214.n.a) ^ (unsigned)(2108834987u))) & 31u))) | 1u))) | 0u))) & 0xffffffffu;
      for (unsigned g21 = 0u; g21 < 8u; g21++) {
        unsigned i20 = g21;
        cs = csmix(cs, i20);
        m215[((unsigned)(u7) & 3u)][((unsigned)(2036217802u) & 3u)] = (unsigned)(((unsigned)(((unsigned)((((unsigned)((-((unsigned)(st13.f2) | 0u))) & 1u) ? (unsigned)((((unsigned)(st12.f1) & 1u) ? (unsigned)(arr10[((unsigned)(i20) & 7u)]) : (unsigned)((**ppa217)))) : (unsigned)(n214.n.b))) - (unsigned)(arr11[((unsigned)(3437511135u) & 7u)]))) * (unsigned)(((unsigned)(n214.n.b) % ((unsigned)(((unsigned)(((unsigned)(st13.f0) << ((unsigned)(u6) & 31u))) / ((unsigned)((**ppa217)) | 1u))) | 1u)))));
        cs = csmix(cs, *(&m215[((unsigned)(u7) & 3u)][0] + ((unsigned)(2036217802u) & 3u)));
        cs = csmix(cs, **ppa217);
        cs = csmix(cs, *pa216);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(arr11[((unsigned)(u5) & 7u)]) >> ((unsigned)(((unsigned)((~((unsigned)(n214.t) | 0u))) - (unsigned)(((unsigned)(u8) <= ((unsigned)(2995785354u) ^ cs))))) & 31u))) < ((unsigned)(((unsigned)((**ppa217)) ^ (unsigned)(u7))) ^ cs))));
      }
      g19++;
    }
  }
  arr11[((unsigned)(u9) & 7u)] = (unsigned)(((unsigned)(((unsigned)((**ppa217)) ^ (unsigned)(((unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(u7) : (unsigned)(3727089694u))) + (unsigned)(((unsigned)(st12.f0) & (unsigned)(n214.t))))))) << ((unsigned)(((unsigned)(m215[((unsigned)(u9) & 3u)][((unsigned)(u9) & 3u)]) ^ (unsigned)(((unsigned)(((unsigned)(m215[((unsigned)(u9) & 3u)][((unsigned)(u8) & 3u)]) % ((unsigned)(3785027396u) | 1u))) + (unsigned)((unsigned)(s2)))))) & 31u)));
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr10[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr11[k]);
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
