/* Regression: switch fuzz seed 14009 (O2 miscompile).
 *
 * sl_forward forwarded stack stores correctly, but its follow-up cleanup could
 * delete the original stores after an exact-offset read scan.  With runtime
 * indexed stack-array accesses in the same function, those stores can still be
 * needed even when no exact local offset operand remains.
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
  return (unsigned)(pa) ^ lr;
}
static unsigned helper2(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  lr = (unsigned)(((unsigned)(((unsigned)(((unsigned)(pa) >> ((unsigned)(((unsigned)(pa) ^ lr)) & 31u))) - (unsigned)(((unsigned)(2893586235u) % ((unsigned)(841457824u) | 1u))))) / ((unsigned)(((unsigned)(((unsigned)(3738749452u) % ((unsigned)(pa) | 1u))) != ((unsigned)(((unsigned)(pb) | (unsigned)(1276722845u))) ^ lr))) | 1u)));
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) & (unsigned)(649361069u))) & (unsigned)(((unsigned)(2260972834u) * (unsigned)(2166336084u))))) ^ (unsigned)(pa))) ^ lr;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  short s3 = (short)(525768537u & 0xffff);
  long s4 = (long)(1436794999u & 0xffffffff);
  int s5 = (int)(838993062u & 0xffffffff);
  unsigned u6 = 3405519172u;
  unsigned u7 = 2304225825u;
  unsigned arr8[8] = { 2875334796u, 774272488u, 1844814402u, 785632954u, 351196070u, 3657772559u, 2277150539u, 995785257u };
  if ((unsigned)((((unsigned)(((unsigned)(((unsigned)(helper1((unsigned)(s4), 2799279203u)) % ((unsigned)((-((unsigned)(3548075109u) | 0u))) | 1u))) - (unsigned)(arr8[((unsigned)(u6) & 7u)]))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(((unsigned)(805062190u) ^ (unsigned)(3984356282u))) ^ (unsigned)(((unsigned)(u7) & (unsigned)(16963033u))))) / ((unsigned)(u7) | 1u))) : (unsigned)(945517228u))) & 1u) {
    cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(helper2((~((unsigned)(u6) | 0u)), u6)) % ((unsigned)((-((unsigned)(((unsigned)(1754617002u) ^ (unsigned)(1978610889u))) | 0u))) | 1u))) - (unsigned)(arr8[((unsigned)(3276170555u) & 7u)]))));
    { unsigned g9 = (unsigned)(((unsigned)(850522906u) / ((unsigned)(((unsigned)(u6) > ((unsigned)((((unsigned)(u6) & 1u) ? (unsigned)(((unsigned)(u6) ^ (unsigned)(1417460814u))) : (unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(u7))))) ^ cs))) | 1u))) & 1u;
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(136432048u) << ((unsigned)((unsigned)(s5)) & 31u))) - (unsigned)((unsigned)(s3)))));
      cs = csmix(cs, (unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) != ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1279621174u) + (unsigned)((unsigned)(s4)))) ^ (unsigned)(((unsigned)(arr8[((unsigned)(2872515691u) & 7u)]) + (unsigned)(2330546543u))))) << ((unsigned)(((unsigned)(u6) | (unsigned)((((unsigned)(1451072783u) & 1u) ? (unsigned)(3540542533u) : (unsigned)(3331385419u))))) & 31u))) ^ cs))));
      cs = csmix(cs, (unsigned)(3751036478u));
      cs = csmix(cs, 93u); }
    { unsigned g11 = 0u;
      while (g11 < 3u) {
        unsigned i10 = g11;
        cs = csmix(cs, i10);
        u7 = (unsigned)(i10) & 0xffffffffu;
        g11++;
      }
    }
  } else {
    for (unsigned g13 = 0u; g13 < 12u; g13++) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      u7 = (unsigned)(u7) & 0xffffffffu;
      arr8[((unsigned)(i12) & 7u)] = (unsigned)((-((unsigned)(3216892597u) | 0u)));
    }
    if ((unsigned)(u7) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1732653086u) + (unsigned)((unsigned)(s4)))) / ((unsigned)(((unsigned)(((unsigned)(u7) ^ (unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) * (unsigned)(u7))))) & (unsigned)(arr8[((unsigned)(u7) & 7u)]))) | 1u))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((~((unsigned)(arr8[((unsigned)(u6) & 7u)]) | 0u))) << ((unsigned)(((unsigned)(arr8[((unsigned)(u6) & 7u)]) & (unsigned)(arr8[((unsigned)(130170635u) & 7u)]))) & 31u))) / ((unsigned)(((unsigned)(((unsigned)(3932181903u) == ((unsigned)(4055030574u) ^ cs))) + (unsigned)(u6))) | 1u))) < ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) | (unsigned)(((unsigned)(u6) ^ cs)))) >> ((unsigned)(3640778313u) & 31u))) / ((unsigned)(((unsigned)(((unsigned)(1181476167u) % ((unsigned)(3628535472u) | 1u))) + (unsigned)(((unsigned)(65359278u) & (unsigned)(arr8[((unsigned)(u7) & 7u)]))))) | 1u))) ^ cs))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(954418415u) - (unsigned)((unsigned)(s5)))) <= ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u6) % ((unsigned)((unsigned)(s3)) | 1u))) % ((unsigned)((-((unsigned)((unsigned)(s3)) | 0u))) | 1u))) + (unsigned)(2783183647u))) ^ cs))));
    }
    { unsigned g14 = (unsigned)(((unsigned)(u7) >> ((unsigned)((-((unsigned)(3271714036u) | 0u))) & 31u))) & 1u;
      cs = csmix(cs, (unsigned)(3547891905u));
      cs = csmix(cs, (unsigned)(((unsigned)(298929380u) & (unsigned)((~((unsigned)((-((unsigned)(((unsigned)((unsigned)(s5)) & (unsigned)(2724688105u))) | 0u))) | 0u))))));
      cs = csmix(cs, 217u); }
    { unsigned g15 = (unsigned)(357464725u) & 1u;
      cs = csmix(cs, (unsigned)(u7));
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(2987151894u) % ((unsigned)(((unsigned)(u7) % ((unsigned)(u6) | 1u))) | 1u))) | 0u))) ^ (unsigned)(u7))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) / ((unsigned)(((unsigned)(((unsigned)((unsigned)(s5)) & (unsigned)(525521984u))) - (unsigned)((~((unsigned)(444375295u) | 0u))))) | 1u))) & (unsigned)(((unsigned)(((unsigned)(helper1((unsigned)(s5), 2418867174u)) % ((unsigned)(((unsigned)(3589880615u) ^ (unsigned)(arr8[((unsigned)(u6) & 7u)]))) | 1u))) << ((unsigned)(4224078127u) & 31u))))));
      cs = csmix(cs, 79u); }
  }
  { unsigned sel16 = (unsigned)(311457093u) & 63u;
    switch (sel16) {
      cs = csmix(cs, 2175659194u);
      { unsigned sel17 = (unsigned)(((unsigned)(1130268021u) == ((unsigned)((unsigned)(s4)) ^ cs))) & 63u;
        switch (sel17) {
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1956521668u) * (unsigned)((~((unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) - (unsigned)(u6))) | 0u))))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(3756839717u) >> ((unsigned)(2255619184u) & 31u))) * (unsigned)((((unsigned)(1521510704u) & 1u) ? (unsigned)(2794650705u) : (unsigned)((unsigned)(s3)))))) + (unsigned)(((unsigned)(helper2(3373251402u, arr8[((unsigned)(271728185u) & 7u)])) / ((unsigned)(((unsigned)(306346456u) >> ((unsigned)(u7) & 31u))) | 1u))))) & 31u))));
          cs = csmix(cs, 3887530974u);
          cs = csmix(cs, 3964693479u);
          cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)((~((unsigned)(1334831193u) | 0u))) | 0u))) == ((unsigned)(((unsigned)(3433788039u) - (unsigned)(2572880984u))) ^ cs))));
          cs = csmix(cs, (unsigned)(arr8[((unsigned)(118560723u) & 7u)]));
          cs = csmix(cs, 2794354238u);
          cs = csmix(cs, 362468173u);
          cs = csmix(cs, 277736666u);
          cs = csmix(cs, (unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u6) ^ cs)))));
          cs = csmix(cs, 2973556356u);
          cs = csmix(cs, 3783034523u);
        default: cs = csmix(cs, 7u); break;
        } }
      cs = csmix(cs, 1345802647u);
      { unsigned g19 = 0u;
        while (g19 < 4u) {
          unsigned i18 = g19;
          cs = csmix(cs, i18);
        }
      }
      cs = csmix(cs, 3889940779u);
      cs = csmix(cs, 1648522941u);
      cs = csmix(cs, 2208691906u);
      cs = csmix(cs, (unsigned)(((unsigned)(2829714011u) * (unsigned)(((unsigned)(((unsigned)((((unsigned)(1135211538u) & 1u) ? (unsigned)(3410705040u) : (unsigned)(u6))) + (unsigned)(((unsigned)(u7) / ((unsigned)((unsigned)(s4)) | 1u))))) / ((unsigned)((unsigned)(s5)) | 1u))))));
      cs = csmix(cs, 2015396834u);
      { unsigned sel20 = (unsigned)(((unsigned)(u6) & (unsigned)(arr8[((unsigned)(3355668953u) & 7u)]))) & 7u;
        switch (sel20) {
          cs = csmix(cs, 3931444679u);
          cs = csmix(cs, 2051558919u);
          cs = csmix(cs, (unsigned)(helper1(((unsigned)((~((unsigned)(u7) | 0u))) + (unsigned)(((unsigned)(((unsigned)(u6) + (unsigned)(505588633u))) % ((unsigned)(u6) | 1u)))), ((unsigned)((~((unsigned)(helper1(u7, u6)) | 0u))) ^ (unsigned)(((unsigned)(((unsigned)(u6) - (unsigned)(1173439798u))) - (unsigned)(u6)))))));
          cs = csmix(cs, 2977488357u);
          cs = csmix(cs, (unsigned)(helper2(1850138800u, ((unsigned)(u7) ^ (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) & (unsigned)(3323775256u))) << ((unsigned)((-((unsigned)(3340424598u) | 0u))) & 31u)))))));
          cs = csmix(cs, 1500887470u);
          cs = csmix(cs, (unsigned)(((unsigned)(u6) * (unsigned)(1708956749u))));
          cs = csmix(cs, 429388970u);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((-((unsigned)(3915598658u) | 0u))) <= ((unsigned)(755479888u) ^ cs))) - (unsigned)(((unsigned)((-((unsigned)(1892786554u) | 0u))) << ((unsigned)(u6) & 31u))))) - (unsigned)((((unsigned)(((unsigned)(((unsigned)(786378346u) | (unsigned)(arr8[((unsigned)(u6) & 7u)]))) ^ (unsigned)(arr8[((unsigned)(2448428697u) & 7u)]))) & 1u) ? (unsigned)((-((unsigned)((((unsigned)((unsigned)(s4)) & 1u) ? (unsigned)(1441885613u) : (unsigned)(arr8[((unsigned)(u7) & 7u)]))) | 0u))) : (unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) ^ (unsigned)(1205177321u))) >= ((unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)(1593113997u))) ^ cs))))))));
          cs = csmix(cs, 187051984u);
          cs = csmix(cs, 139917231u);
          cs = csmix(cs, 1879810807u);
        default: cs = csmix(cs, 170u); break;
        } }
      cs = csmix(cs, 2749201544u);
      for (unsigned g22 = 0u; g22 < 3u; g22++) {
        unsigned i21 = g22;
        cs = csmix(cs, i21);
        cs = csmix(cs, (unsigned)(((unsigned)(3902708828u) >> ((unsigned)(helper1(arr8[((unsigned)(u7) & 7u)], ((unsigned)(((unsigned)((unsigned)(s3)) << ((unsigned)(((unsigned)((unsigned)(s3)) ^ cs)) & 31u))) + (unsigned)(((unsigned)((unsigned)(s5)) + (unsigned)(arr8[((unsigned)(u7) & 7u)])))))) & 31u))));
      }
      cs = csmix(cs, 980691199u);
      { unsigned g24 = 0u;
        while (g24 < 11u) {
          unsigned i23 = g24;
          cs = csmix(cs, i23);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(3619587967u) % ((unsigned)(((unsigned)(i23) - (unsigned)((unsigned)(s4)))) | 1u))) << ((unsigned)(((unsigned)((unsigned)(s5)) % ((unsigned)(((unsigned)(u6) / ((unsigned)(arr8[((unsigned)(u6) & 7u)]) | 1u))) | 1u))) & 31u))) >> ((unsigned)((unsigned)(s4)) & 31u))));
          cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) ^ (unsigned)(arr8[((unsigned)(2668823274u) & 7u)]))) ^ (unsigned)(((unsigned)(helper1(u6, u6)) << ((unsigned)(((unsigned)(76521310u) % ((unsigned)((unsigned)(s4)) | 1u))) & 31u))))) | 0u))));
          cs = csmix(cs, (unsigned)((~((unsigned)(581524667u) | 0u))));
        }
      }
      cs = csmix(cs, 2290996983u);
      cs = csmix(cs, 2856308199u);
    default: cs = csmix(cs, 162u); break;
    } }
  cs = csmix(cs, (unsigned)((unsigned)(s3)));
  { unsigned g26 = 0u;
    while (g26 < 8u) {
      unsigned i25 = g26;
      cs = csmix(cs, i25);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(i25) % ((unsigned)(3343022491u) | 1u))) * (unsigned)(((unsigned)(((unsigned)((((unsigned)(i25) & 1u) ? (unsigned)(i25) : (unsigned)(2653989015u))) % ((unsigned)(helper1(794375936u, 3013849065u)) | 1u))) | (unsigned)(helper2(((unsigned)(377441164u) - (unsigned)(i25)), ((unsigned)(i25) & (unsigned)(u6)))))))));
      { unsigned g27 = (unsigned)((((unsigned)(((unsigned)(2743619616u) >> ((unsigned)(((unsigned)(arr8[((unsigned)(4275689140u) & 7u)]) - (unsigned)(u6))) & 31u))) & 1u) ? (unsigned)(u6) : (unsigned)(((unsigned)(i25) | (unsigned)(arr8[((unsigned)(u6) & 7u)]))))) & 1u;
        cs = csmix(cs, (unsigned)(334733544u));
        cs = csmix(cs, 151u); }
      for (unsigned g29 = 0u; g29 < 10u; g29++) {
        unsigned i28 = g29;
        cs = csmix(cs, i28);
        cs = csmix(cs, (unsigned)(2213339498u));
        cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(i28) | 0u))) + (unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s4)) << ((unsigned)((unsigned)(s5)) & 31u))) % ((unsigned)(i28) | 1u))) << ((unsigned)(((unsigned)(((unsigned)((unsigned)(s3)) / ((unsigned)(((unsigned)((unsigned)(s3)) ^ cs)) | 1u))) & (unsigned)(735524117u))) & 31u))))));
      }
      g26++;
    }
  }
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, helper2(19088744u, cs));
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  printf("checksum=%08x\n", cs);
  return 0;
}
