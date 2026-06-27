/* Regression test (verbatim differential-fuzz repro, gen_c.py seed=51).
 * ssa_opt_dead_loop folded a loop-carried header phi to its latch constant because dead_loop_body_hi under-counted a split/rotated back-edge body (fix: ir/opt/ssa_opt_dead_loop.c).
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
  lr = (unsigned)(((unsigned)(((unsigned)((-((unsigned)(lr) | 0u))) / ((unsigned)(((unsigned)(pb) != ((unsigned)(((unsigned)(pb) ^ lr)) ^ lr))) | 1u))) >> ((unsigned)(((unsigned)(pa) & (unsigned)(((unsigned)(lr) ^ (unsigned)(pb))))) & 31u)));
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(pa) ^ (unsigned)(4235552571u))) + (unsigned)(((unsigned)(4061887861u) & (unsigned)(2935160282u))))) << ((unsigned)(((unsigned)((((unsigned)(pa) & 1u) ? (unsigned)(3340477297u) : (unsigned)(pb))) << ((unsigned)(4218955527u) & 31u))) & 31u)));
  if ((unsigned)(3495471273u) & 1u) lr += (unsigned)(1406881740u);
  return (unsigned)((~((unsigned)(((unsigned)(lr) / ((unsigned)(((unsigned)(pb) - (unsigned)(lr))) | 1u))) | 0u))) ^ lr;
}

struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};

int main(void)
{
  unsigned cs = 0x12345678u;
  long s2 = (long)(871508908u & 0xffffffff);
  char s3 = (char)(1264742730u & 0xff);
  char s4 = (char)(2067705761u & 0xff);
  unsigned u5 = 3798098143u;
  unsigned u6 = 766736778u;
  unsigned u7 = 1280340641u;
  unsigned arr8[8] = { 2836044892u, 2909791686u, 3117596330u, 2871026039u, 3128131752u, 2052504332u, 1199434395u, 3335126204u };
  unsigned arr9[8] = { 1657425900u, 1374363168u, 2945931366u, 2373513731u, 1393439082u, 2604511850u, 562311347u, 1772577504u };

  for (unsigned g11 = 0u; g11 < 1u; g11++) {
    unsigned i10 = g11;
    cs = csmix(cs, i10);
    if ((unsigned)(u5) & 1u) {
      cs = csmix(cs, (unsigned)(u6));
      cs = csmix(cs, (unsigned)(((unsigned)(u5) / ((unsigned)(u6) | 1u))));
      u5 = (unsigned)(((unsigned)(arr8[((unsigned)(i10) & 7u)]) ^ (unsigned)(3634451293u))) & 0xffffffffu;
    } else {
      arr8[((unsigned)(u5) & 7u)] = (unsigned)(((unsigned)(((unsigned)(helper1(288164220u, u6)) << ((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(((unsigned)((unsigned)(s2)) ^ cs)))) & 31u))) | (unsigned)(3998132334u)));
    }
    if ((unsigned)((unsigned)(s4)) & 1u) {
      u6 = (unsigned)(3349261731u) & 0xffffffffu;
      u5 = (unsigned)((-((unsigned)(arr9[((unsigned)(u5) & 7u)]) | 0u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(1304131043u));
    } else {
      arr8[((unsigned)(u7) & 7u)] = (unsigned)(((unsigned)(((unsigned)((~((unsigned)(((unsigned)(2922968520u) % ((unsigned)(1951570908u) | 1u))) | 0u))) & (unsigned)(((unsigned)(u5) ^ (unsigned)((~((unsigned)(u5) | 0u))))))) != ((unsigned)(u7) ^ cs)));
      cs = csmix(cs, (unsigned)(i10));
      u5 = (unsigned)(u6) & 0xffffffffu;
      u7 = (unsigned)(((unsigned)(arr9[((unsigned)(849237605u) & 7u)]) << ((unsigned)((-((unsigned)((((unsigned)(2617078951u) & 1u) ? (unsigned)(((unsigned)(1216577691u) % ((unsigned)(953453575u) | 1u))) : (unsigned)(arr8[((unsigned)(2048107841u) & 7u)]))) | 0u))) & 31u))) & 0xffffffffu;
      u6 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(210867926u) & 7u)]) <= ((unsigned)(arr9[((unsigned)(u6) & 7u)]) ^ cs))) >> ((unsigned)(arr9[((unsigned)(i10) & 7u)]) & 31u))) / ((unsigned)(((unsigned)(((unsigned)((~((unsigned)(2766277626u) | 0u))) ^ (unsigned)(4226737745u))) <= ((unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(u5) & 7u)]) % ((unsigned)(2639848369u) | 1u))) | (unsigned)((((unsigned)(i10) & 1u) ? (unsigned)(i10) : (unsigned)(arr9[((unsigned)(u5) & 7u)]))))) ^ cs))) | 1u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) & (unsigned)(u7))) >> ((unsigned)(((unsigned)(u6) & (unsigned)(arr8[((unsigned)(u5) & 7u)]))) & 31u))) & (unsigned)(u7))) >> ((unsigned)(i10) & 31u))));
    }
    cs = csmix(cs, (unsigned)(((unsigned)(4134000992u) != ((unsigned)(helper1(((unsigned)(((unsigned)((unsigned)(s3)) & (unsigned)(u6))) > ((unsigned)(((unsigned)(1515894755u) << ((unsigned)((unsigned)(s3)) & 31u))) ^ cs)), ((unsigned)(((unsigned)(2388761311u) / ((unsigned)(i10) | 1u))) + (unsigned)(2564533588u)))) ^ cs))));
    u6 = (unsigned)(u6) & 0xffffffffu;
    if ((unsigned)((unsigned)(s3)) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s2)) + (unsigned)((unsigned)(s3)))));
      cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)(1625158676u) + (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(arr8[((unsigned)(u5) & 7u)]) | 1u))) % ((unsigned)((~((unsigned)(1159414607u) | 0u))) | 1u))))) | 0u))));
      u5 = (unsigned)(((unsigned)(i10) < ((unsigned)((unsigned)(s4)) ^ cs))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)((((unsigned)(((unsigned)(((unsigned)(u6) / ((unsigned)(arr8[((unsigned)(u7) & 7u)]) | 1u))) ^ (unsigned)(((unsigned)(3778774238u) % ((unsigned)(u5) | 1u))))) & 1u) ? (unsigned)(arr9[((unsigned)(u6) & 7u)]) : (unsigned)(((unsigned)(3484310024u) & (unsigned)(u7))))) % ((unsigned)(((unsigned)((-((unsigned)(u5) | 0u))) % ((unsigned)((unsigned)(s4)) | 1u))) | 1u))));
    } else {
      u7 = (unsigned)(((unsigned)((~((unsigned)((unsigned)(s2)) | 0u))) % ((unsigned)(i10) | 1u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(i10));
      u6 = (unsigned)((((unsigned)(((unsigned)(u5) >> ((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) * (unsigned)(i10))) & (unsigned)(((unsigned)(4224675959u) - (unsigned)(u6))))) & 31u))) & 1u) ? (unsigned)(u7) : (unsigned)(((unsigned)(u5) ^ (unsigned)((-((unsigned)(((unsigned)(648854446u) >> ((unsigned)(3068194703u) & 31u))) | 0u))))))) & 0xffffffffu;
      u5 = (unsigned)(helper1(u7, arr8[((unsigned)(i10) & 7u)])) & 0xffffffffu;
      u5 = (unsigned)(2429107059u) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(((unsigned)(2200017142u) - (unsigned)(((unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(1210527945u) & 7u)]) / ((unsigned)(1085331311u) | 1u))) & (unsigned)((unsigned)(s4)))) >> ((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) + (unsigned)(((unsigned)((unsigned)(s4)) ^ cs)))) + (unsigned)(u5))) & 31u))))));
    }
  }
  { unsigned g13 = 0u;
    while (g13 < 9u) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      if ((unsigned)((unsigned)(s3)) & 1u) {
        u7 = (unsigned)(((unsigned)(arr9[((unsigned)(u5) & 7u)]) >> ((unsigned)(((unsigned)(((unsigned)(1882236321u) * (unsigned)(1018186252u))) >> ((unsigned)(arr9[((unsigned)(i12) & 7u)]) & 31u))) & 31u))) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(arr9[((unsigned)(i12) & 7u)]) + (unsigned)(i12))) + (unsigned)(807645287u))) + (unsigned)(((unsigned)(488267022u) / ((unsigned)(((unsigned)(u5) + (unsigned)(((unsigned)(u5) ^ cs)))) | 1u))))) ^ (unsigned)(3102497187u))));
        cs = csmix(cs, (unsigned)((unsigned)(s3)));
        arr8[((unsigned)(2164312748u) & 7u)] = (unsigned)((-((unsigned)((((unsigned)(((unsigned)((unsigned)(s4)) ^ (unsigned)(((unsigned)(1663221652u) <= ((unsigned)(arr9[((unsigned)(187273668u) & 7u)]) ^ cs))))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(1282462997u) ^ (unsigned)(u6))) ^ (unsigned)(u6))) : (unsigned)(u6))) | 0u)));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(2954658433u) & (unsigned)((~((unsigned)(((unsigned)(3779205137u) | (unsigned)(1513938857u))) | 0u))))) << ((unsigned)(helper1(((unsigned)(2835515906u) & (unsigned)(((unsigned)(3215459291u) % ((unsigned)(u5) | 1u)))), u7)) & 31u))));
      } else {
        arr8[((unsigned)(i12) & 7u)] = (unsigned)(2285918764u);
      }
      cs = csmix(cs, (unsigned)((~((unsigned)(helper1(1662820696u, 3482249617u)) | 0u))));
      g13++;
    }
  }
  u6 = (unsigned)(((unsigned)(4071453809u) / ((unsigned)((~((unsigned)(helper1(helper1((unsigned)(s4), 1079899823u), u6)) | 0u))) | 1u))) & 0xffffffffu;
  cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(u5) | 0u))) * (unsigned)(((unsigned)(((unsigned)(3529523472u) != ((unsigned)(helper1(u7, u6)) ^ cs))) / ((unsigned)(((unsigned)(((unsigned)(u5) * (unsigned)(u7))) + (unsigned)(u7))) | 1u))))));

  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr9[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
