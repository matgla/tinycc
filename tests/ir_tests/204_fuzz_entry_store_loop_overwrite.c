/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=295).
 * entry_store_prop (ir/opt_memory.c) forwarded a stale entry-BB array
 * initializer into a loop-interior read of the SAME element even though that
 * element is overwritten every iteration.  A recent guard had kept the entry
 * store in the forwarding table whenever a runtime-indexed LOAD_INDEXED covered
 * its offset ("protected_by_rt_li") -- but that table only drives constant
 * forwarding; runtime loads read memory directly.  As a result, a loop-interior
 * deref like `*(&arr12[3])` was folded to the initial constant `#161752171`
 * despite `arr12[3]` being stored each iteration, so iterations 1+ observed the
 * wrong value.  Fix: never forward an entry-BB store whose offset is written
 * after the entry BB (a back-edge may reach the load after the overwrite).
 * tcc -O0 was always correct; the bug appeared at -O1/-O2.  Expected checksum
 * is gcc -m32 -funsigned-char (ARM ABI: unsigned char, 32-bit long).
 */
#include <stdio.h>

/* Rolling checksum mix (all unsigned -> fully defined). */
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}


static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(1108790020u);
  if ((unsigned)(((unsigned)(3188871538u) - (unsigned)(959688269u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(559739026u) + (unsigned)(pa))) ^ (unsigned)(((unsigned)(3249589323u) & (unsigned)(pb)))));
  lr = (unsigned)(2039757517u);
  lr = (unsigned)(pa);
  if ((unsigned)(((unsigned)(pa) >> ((unsigned)(((unsigned)(1040227394u) + (unsigned)(pa))) & 31u))) & 1u) lr += (unsigned)(((unsigned)(((unsigned)(3323052376u) % ((unsigned)(3061995621u) | 1u))) ^ (unsigned)(2478262266u)));
  return (unsigned)((~((unsigned)(79203140u) | 0u))) ^ lr;
}

static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)((~((unsigned)(((unsigned)(pa) << ((unsigned)((~((unsigned)(lr) | 0u))) & 31u))) | 0u)));
  if ((unsigned)(((unsigned)(3105000289u) > ((unsigned)(((unsigned)(pa) / ((unsigned)(3348456148u) | 1u))) ^ lr))) & 1u) lr += (unsigned)(((unsigned)(pb) << ((unsigned)(((unsigned)(128492937u) | (unsigned)(pa))) & 31u)));
  if ((unsigned)(pa) & 1u) lr += (unsigned)(((unsigned)((~((unsigned)(1795631888u) | 0u))) < ((unsigned)(((unsigned)(2896355668u) & (unsigned)(1447309716u))) ^ lr)));
  return (unsigned)(((unsigned)(pb) - (unsigned)(((unsigned)(pb) ^ lr)))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  long s3 = (long)(1200119061u & 0xffffffff);
  short s4 = (short)(896884488u & 0xffff);
  int s5 = (int)(1504507546u & 0xffffffff);
  unsigned u6 = 2800720541u;
  unsigned u7 = 947988770u;
  unsigned u8 = 1336075728u;
  unsigned u9 = 679072998u;
  unsigned u10 = 1294708143u;
  unsigned u11 = 277617283u;
  unsigned arr12[8] = { 577038586u, 1947215736u, 1677458213u, 161752171u, 3041148399u, 830570387u, 2244113235u, 3378818769u };
  unsigned arr13[8] = { 64624351u, 3023046793u, 3539630400u, 3071517219u, 3564524048u, 1408472201u, 36201267u, 3052409330u };
  struct S st14 = { 3664991592u, 122353467u, 4088400823u };
  struct S st15 = { 2592266235u, 2313165272u, 4084013653u };

  u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(1537936369u) * (unsigned)(u10))) % ((unsigned)(((unsigned)((~((unsigned)(arr12[((unsigned)(1706791926u) & 7u)]) | 0u))) ^ (unsigned)(arr13[((unsigned)(4073544685u) & 7u)]))) | 1u))) > ((unsigned)(((unsigned)(arr12[((unsigned)(u8) & 7u)]) << ((unsigned)((((unsigned)(((unsigned)(450847187u) % ((unsigned)(st15.f2) | 1u))) & 1u) ? (unsigned)(((unsigned)(u10) & (unsigned)((unsigned)(s3)))) : (unsigned)(((unsigned)(u8) + (unsigned)(3009363269u))))) & 31u))) ^ cs))) & 0xffffffffu;
  if ((unsigned)(((unsigned)(u11) | (unsigned)(st14.f2))) & 1u) {
    cs = csmix(cs, (unsigned)(u6));
    u10 = (unsigned)(((unsigned)(((unsigned)(826352469u) ^ (unsigned)(((unsigned)((~((unsigned)(st14.f2) | 0u))) / ((unsigned)(arr13[((unsigned)(476705523u) & 7u)]) | 1u))))) / ((unsigned)(((unsigned)(((unsigned)((~((unsigned)(2329892609u) | 0u))) >> ((unsigned)(((unsigned)(2464372371u) / ((unsigned)((unsigned)(s5)) | 1u))) & 31u))) << ((unsigned)(((unsigned)(2648670121u) << ((unsigned)((unsigned)(s5)) & 31u))) & 31u))) | 1u))) & 0xffffffffu;
    { unsigned g17 = 0u;
      while (g17 < 9u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, (unsigned)(u9));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(arr13[((unsigned)(u8) & 7u)]) | (unsigned)(u6))) >> ((unsigned)((((unsigned)(((unsigned)((-((unsigned)(2512260433u) | 0u))) + (unsigned)(arr12[((unsigned)(u7) & 7u)]))) & 1u) ? (unsigned)(arr12[((unsigned)(u11) & 7u)]) : (unsigned)((~((unsigned)(st15.f0) | 0u))))) & 31u))));
        arr12[((unsigned)(3444153571u) & 7u)] = (unsigned)(((unsigned)(656604400u) << ((unsigned)(((unsigned)((~((unsigned)(((unsigned)(3346358622u) + (unsigned)((unsigned)(s3)))) | 0u))) / ((unsigned)(((unsigned)(((unsigned)(2265289242u) | (unsigned)(u10))) - (unsigned)(3133518581u))) | 1u))) & 31u)));
        cs = csmix(cs, (unsigned)((unsigned)(s4)));
        st14.f1 = (unsigned)(arr12[((unsigned)(1312178992u) & 7u)]);
        u8 = (unsigned)((unsigned)(s5)) & 0xffffffffu;
        g17++;
      }
    }
    st14.f0 = (unsigned)(((unsigned)(((unsigned)(helper1(((unsigned)(arr13[((unsigned)(u9) & 7u)]) / ((unsigned)(arr12[((unsigned)(u11) & 7u)]) | 1u)), ((unsigned)(u9) >> ((unsigned)(arr13[((unsigned)(2310870051u) & 7u)]) & 31u)))) % ((unsigned)(arr13[((unsigned)(u8) & 7u)]) | 1u))) - (unsigned)(1589328448u)));
  }
  arr12[((unsigned)(u9) & 7u)] = (unsigned)((-((unsigned)(arr12[((unsigned)(u8) & 7u)]) | 0u)));
  cs = csmix(cs, (unsigned)(((unsigned)(st15.f0) | (unsigned)(((unsigned)(2731854347u) + (unsigned)((((unsigned)(st15.f0) & 1u) ? (unsigned)(u9) : (unsigned)(((unsigned)(u9) >> ((unsigned)(u7) & 31u))))))))));
  u6 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(u11) | 0u))) % ((unsigned)(arr13[((unsigned)(1255840387u) & 7u)]) | 1u))) << ((unsigned)(4044470401u) & 31u))) & (unsigned)(st15.f0))) & 0xffffffffu;

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
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr13[k]);
  cs = csmix(cs, st14.f0);
  cs = csmix(cs, st14.f1);
  cs = csmix(cs, st14.f2);
  cs = csmix(cs, st15.f0);
  cs = csmix(cs, st15.f1);
  cs = csmix(cs, st15.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
