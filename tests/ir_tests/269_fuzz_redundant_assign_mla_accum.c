/* Fuzz regression (switch profile seed 10003, ptr seed 19825; O1/O2 miscompile):
 * redundant_var_assign (ir/opt_dce.c) tracked "last unread ASSIGN to a VAR" via
 * src1/src2 reads only, so a VAR read as an MLA *accumulator* (4th operand,
 * created by mla-fusion) looked unread and the pass NOP'd its live defining
 * load:  V3 <-- StackLoc[-12]; T <-- Ta MLA Tb + V3; V3 <-- T OR #k  deleted
 * the load, feeding a stale V3 into the MLA.
 * Fix: clear the pending-assign entry on MLA accumulator reads too. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(pb) % ((unsigned)(((unsigned)(605749797u) * (unsigned)(2089241154u))) | 1u))) + (unsigned)(((unsigned)(((unsigned)(pb) << ((unsigned)(2921809617u) & 31u))) / ((unsigned)(((unsigned)(2378713918u) + (unsigned)(435361911u))) | 1u))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(1030745780u & 0xffffffff);
  unsigned u3 = 3468218738u;
  unsigned u4 = 1749707605u;
  unsigned u5 = 3544643165u;
  struct S st6 = { 1877174992u, 163391553u, 313104196u };
  struct S st7 = { 395737445u, 2088261493u, 3764519566u };
  { unsigned sel8 = (unsigned)(u5) & 3u;
    switch (sel8) {
    case 0:
      if ((unsigned)((unsigned)(s2)) & 1u) {
        cs = csmix(cs, (unsigned)(2865735267u));
      }
      for (unsigned g10 = 0u; g10 < 3u; g10++) {
        unsigned i9 = g10;
        cs = csmix(cs, i9);
        u5 = (unsigned)(3374991661u) & 0xffffffffu;
        cs = csmix(cs, (unsigned)(((unsigned)(helper1(((unsigned)(((unsigned)(u5) * (unsigned)(st6.f1))) * (unsigned)(((unsigned)(st6.f0) >> ((unsigned)(u4) & 31u)))), ((unsigned)(((unsigned)(i9) | (unsigned)(((unsigned)(i9) ^ cs)))) == ((unsigned)((((unsigned)(st6.f2) & 1u) ? (unsigned)(4034042790u) : (unsigned)(u5))) ^ cs)))) ^ (unsigned)(helper1(((unsigned)(((unsigned)(st6.f0) + (unsigned)((unsigned)(s2)))) ^ (unsigned)(((unsigned)(3007748529u) % ((unsigned)(3080667385u) | 1u)))), ((unsigned)(i9) + (unsigned)((unsigned)(s2))))))));
      }
      cs = csmix(cs, 3013459160u);
      cs = csmix(cs, 1394186841u);
      cs = csmix(cs, 3803019565u);
      cs = csmix(cs, (unsigned)(2807596651u));
      cs = csmix(cs, 1007473797u);
    default: cs = csmix(cs, 98u); break;
    } }
  cs = csmix(cs, (unsigned)(3172853855u));
  { unsigned g12 = 0u;
    while (g12 < 10u) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      { unsigned sel13 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2809915616u) << ((unsigned)(st6.f2) & 31u))) & (unsigned)(((unsigned)(u3) ^ (unsigned)(1008188185u))))) >= ((unsigned)(((unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(u3))) / ((unsigned)(1213830450u) | 1u))) ^ cs))) << ((unsigned)((-((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(u3))) << ((unsigned)(((unsigned)(u3) << ((unsigned)(st6.f2) & 31u))) & 31u))) | 0u))) & 31u))) & 7u;
        switch (sel13) {
          cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(1578768931u) | 0u))) << ((unsigned)(((unsigned)(u5) % ((unsigned)(i11) | 1u))) & 31u))));
          cs = csmix(cs, 2142316300u);
          cs = csmix(cs, (unsigned)(((unsigned)(u5) * (unsigned)(((unsigned)((~((unsigned)((((unsigned)(u3) & 1u) ? (unsigned)(131187920u) : (unsigned)(u4))) | 0u))) % ((unsigned)(((unsigned)(((unsigned)(u3) % ((unsigned)(2939950116u) | 1u))) / ((unsigned)(((unsigned)(3118508921u) ^ (unsigned)(u5))) | 1u))) | 1u))))));
          cs = csmix(cs, 1858708618u);
          cs = csmix(cs, 556968696u);
          cs = csmix(cs, 2783121068u);
          cs = csmix(cs, 3423855455u);
          cs = csmix(cs, 1488023662u);
        default: cs = csmix(cs, 47u); break;
        } }
      g12++;
    }
  }
  { unsigned sel14 = (unsigned)(((unsigned)((~((unsigned)(((unsigned)(u5) & (unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(st6.f2))))) | 0u))) - (unsigned)((unsigned)(s2)))) & 7u;
    switch (sel14) {
      cs = csmix(cs, 1270686931u);
      for (unsigned g16 = 0u; g16 < 11u; g16++) {
        unsigned i15 = g16;
        cs = csmix(cs, i15);
        cs = csmix(cs, (unsigned)(((unsigned)(3662983659u) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) << ((unsigned)(u4) & 31u))) + (unsigned)(((unsigned)(2707064763u) / ((unsigned)(st7.f1) | 1u))))) | (unsigned)((-((unsigned)(((unsigned)(u5) ^ (unsigned)(st6.f2))) | 0u))))) & 31u))));
        cs = csmix(cs, (unsigned)(st6.f2));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((~((unsigned)(495252721u) | 0u))) ^ (unsigned)(((unsigned)(i15) + (unsigned)((~((unsigned)(u5) | 0u))))))) * (unsigned)((-((unsigned)(i15) | 0u))))));
      }
      { unsigned g18 = 0u;
        while (g18 < 2u) {
          unsigned i17 = g18;
          cs = csmix(cs, i17);
          cs = csmix(cs, (unsigned)(st6.f2));
        }
      }
      cs = csmix(cs, 444124006u);
      { unsigned sel19 = (unsigned)(((unsigned)(((unsigned)(st6.f0) << ((unsigned)(u3) & 31u))) >> ((unsigned)(651050784u) & 31u))) & 63u;
        switch (sel19) {
          cs = csmix(cs, 3534284605u);
          cs = csmix(cs, 4016117771u);
          cs = csmix(cs, (unsigned)(helper1(((unsigned)(u4) >= ((unsigned)((~((unsigned)(u5) | 0u))) ^ cs)), ((unsigned)((~((unsigned)(((unsigned)(1637952091u) - (unsigned)(st6.f0))) | 0u))) / ((unsigned)((~((unsigned)(((unsigned)(1954494333u) - (unsigned)(1781398166u))) | 0u))) | 1u)))));
          cs = csmix(cs, 755931483u);
          cs = csmix(cs, 1090114394u);
        default: cs = csmix(cs, 92u); break;
        } }
      cs = csmix(cs, 3270219131u);
      { unsigned sel20 = (unsigned)(((unsigned)(((unsigned)((~((unsigned)((~((unsigned)(st6.f2) | 0u))) | 0u))) + (unsigned)(((unsigned)(1833487415u) * (unsigned)(984599128u))))) - (unsigned)(((unsigned)(helper1(1361931993u, 1121725596u)) >> ((unsigned)(((unsigned)(((unsigned)(st7.f2) * (unsigned)(4186062469u))) << ((unsigned)(2994041142u) & 31u))) & 31u))))) & 63u;
        switch (sel20) {
          cs = csmix(cs, 3609291494u);
          cs = csmix(cs, 2594702819u);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u3) << ((unsigned)(((unsigned)(((unsigned)(3775715949u) % ((unsigned)(395706104u) | 1u))) / ((unsigned)((unsigned)(s2)) | 1u))) & 31u))) / ((unsigned)(u5) | 1u))));
          cs = csmix(cs, (unsigned)(st7.f1));
          cs = csmix(cs, 169125038u);
          cs = csmix(cs, 1582453657u);
          cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(st6.f0) % ((unsigned)((unsigned)(s2)) | 1u))) | 0u))) & (unsigned)(st6.f0))));
          cs = csmix(cs, 424398052u);
          cs = csmix(cs, 3594806807u);
          cs = csmix(cs, (unsigned)(1450698724u));
          cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) & (unsigned)(3543253301u))) << ((unsigned)(u3) & 31u))) ^ (unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(((unsigned)(2620488494u) - (unsigned)(u5))))))) | 0u))));
          cs = csmix(cs, 3612612091u);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(1367919853u) >> ((unsigned)(((unsigned)(((unsigned)(u3) ^ (unsigned)(((unsigned)(u3) ^ cs)))) | (unsigned)((~((unsigned)(892839472u) | 0u))))) & 31u))) ^ (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(((unsigned)(714928500u) - (unsigned)(3322958209u))) | 1u))) >> ((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) ^ (unsigned)(2275422849u))) != ((unsigned)((unsigned)(s2)) ^ cs))) & 31u))))));
          cs = csmix(cs, 461590610u);
          cs = csmix(cs, (unsigned)(165171298u));
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) * (unsigned)(((unsigned)(u3) ^ cs)))) - (unsigned)(helper1(((unsigned)(244266215u) >> ((unsigned)(u4) & 31u)), ((unsigned)(st6.f0) + (unsigned)(2152036830u)))))) | (unsigned)(u5))));
          cs = csmix(cs, 40083700u);
          cs = csmix(cs, 2096003275u);
        default: cs = csmix(cs, 219u); break;
        } }
      if ((unsigned)(((unsigned)((((unsigned)((unsigned)(s2)) & 1u) ? (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(u4))) | (unsigned)((unsigned)(s2)))) : (unsigned)(((unsigned)(115764048u) - (unsigned)(((unsigned)(4219527645u) + (unsigned)(4225597568u))))))) | (unsigned)((unsigned)(s2)))) & 1u) {
        cs = csmix(cs, (unsigned)(u3));
      }
      cs = csmix(cs, 3971163474u);
      if ((unsigned)((~((unsigned)(u4) | 0u))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(u3) & (unsigned)(561390350u))) / ((unsigned)((((unsigned)(helper1(((unsigned)((unsigned)(s2)) / ((unsigned)(st7.f1) | 1u)), ((unsigned)(u3) / ((unsigned)(u5) | 1u)))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(u5) | (unsigned)(u4))) - (unsigned)(u3))) : (unsigned)((((unsigned)(((unsigned)(2860304938u) << ((unsigned)(st6.f2) & 31u))) & 1u) ? (unsigned)(((unsigned)((unsigned)(s2)) * (unsigned)(3515035776u))) : (unsigned)(((unsigned)(319187191u) - (unsigned)((unsigned)(s2)))))))) | 1u))));
        cs = csmix(cs, (unsigned)((~((unsigned)(((unsigned)(st6.f0) + (unsigned)((-((unsigned)(u5) | 0u))))) | 0u))));
      }
      { unsigned sel21 = (unsigned)((((unsigned)(((unsigned)(((unsigned)(3391543664u) + (unsigned)(st7.f0))) | (unsigned)(((unsigned)((unsigned)(s2)) * (unsigned)(((unsigned)(680409158u) ^ (unsigned)(450862789u))))))) & 1u) ? (unsigned)((-((unsigned)(((unsigned)(3447548986u) >> ((unsigned)(helper1((unsigned)(s2), u3)) & 31u))) | 0u))) : (unsigned)(u4))) & 63u;
        switch (sel21) {
          cs = csmix(cs, 487006298u);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(u3) & (unsigned)(((unsigned)(u5) ^ (unsigned)(u3))))) ^ (unsigned)(1719002695u))) - (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) - (unsigned)(((unsigned)(3405251906u) - (unsigned)(u5))))) - (unsigned)(628679692u))))));
          cs = csmix(cs, 3270818621u);
          cs = csmix(cs, (unsigned)(u4));
          cs = csmix(cs, 436865694u);
          cs = csmix(cs, 1201830091u);
          cs = csmix(cs, 1361897733u);
          cs = csmix(cs, 4267109478u);
        default: cs = csmix(cs, 255u); break;
        } }
      cs = csmix(cs, 853349488u);
      { unsigned g22 = (unsigned)(((unsigned)(st6.f0) - (unsigned)(((unsigned)(((unsigned)(49037456u) * (unsigned)(((unsigned)(st7.f2) & (unsigned)((unsigned)(s2)))))) * (unsigned)(u4))))) & 1u;
        cs = csmix(cs, (unsigned)(u3));
        cs = csmix(cs, 76u); }
      { unsigned g23 = (unsigned)(4279642143u) & 1u;
        cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(4290656875u) << ((unsigned)(((unsigned)(st6.f0) ^ (unsigned)(1583850330u))) & 31u))) | 0u))) >> ((unsigned)(st6.f2) & 31u))));
        cs = csmix(cs, 172u); }
      cs = csmix(cs, 809554424u);
      { unsigned sel24 = (unsigned)(2055037600u) & 7u;
        switch (sel24) {
          cs = csmix(cs, 161157099u);
          cs = csmix(cs, 3534843424u);
          cs = csmix(cs, 1837429109u);
          cs = csmix(cs, 4165347474u);
          cs = csmix(cs, (unsigned)(2480561731u));
          cs = csmix(cs, 1703057281u);
          cs = csmix(cs, (unsigned)(u3));
          cs = csmix(cs, 1203701234u);
          cs = csmix(cs, 2045900981u);
          cs = csmix(cs, 1135599758u);
        default: cs = csmix(cs, 36u); break;
        } }
      cs = csmix(cs, 2175056141u);
    case 7:
      if ((unsigned)(((unsigned)(((unsigned)((-((unsigned)(((unsigned)(3080499375u) / ((unsigned)(3345613085u) | 1u))) | 0u))) << ((unsigned)(u3) & 31u))) << ((unsigned)((unsigned)(s2)) & 31u))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(helper1((unsigned)(s2), ((unsigned)(st7.f0) / ((unsigned)((~((unsigned)(u5) | 0u))) | 1u)))) - (unsigned)(((unsigned)(((unsigned)(3047365334u) >> ((unsigned)(((unsigned)(st6.f1) == ((unsigned)(u4) ^ cs))) & 31u))) < ((unsigned)(1517007221u) ^ cs))))));
      } else {
        u4 = (unsigned)(st6.f0) & 0xffffffffu;
        u4 = (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(2128467630u) & 31u))) | (unsigned)(((unsigned)(u4) + (unsigned)(((unsigned)(((unsigned)(st7.f1) << ((unsigned)(2053308210u) & 31u))) * (unsigned)(((unsigned)(3263473629u) * (unsigned)(u5))))))))) & 0xffffffffu;
      }
      if ((unsigned)((unsigned)(s2)) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(((unsigned)(1520748485u) % ((unsigned)(762500698u) | 1u))) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(((unsigned)(u4) ^ (unsigned)(st7.f2))))) % ((unsigned)((~((unsigned)(((unsigned)(u3) == ((unsigned)(3837305335u) ^ cs))) | 0u))) | 1u))) / ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1380543057u) >> ((unsigned)(st6.f1) & 31u))) | (unsigned)(((unsigned)(u4) / ((unsigned)(u5) | 1u))))) % ((unsigned)(((unsigned)(st6.f0) - (unsigned)(st7.f2))) | 1u))) | 1u))));
      }
      cs = csmix(cs, 3245775724u);
    default: cs = csmix(cs, 31u); break;
    } }
  { unsigned sel25 = (unsigned)(st6.f2) & 7u;
    switch (sel25) {
      if ((unsigned)(((unsigned)(((unsigned)(st6.f1) * (unsigned)(((unsigned)(((unsigned)(u4) % ((unsigned)(u5) | 1u))) ^ (unsigned)(3439982432u))))) * (unsigned)(u5))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(2385684503u) ^ (unsigned)(((unsigned)(((unsigned)(st6.f1) * (unsigned)(1090791197u))) & (unsigned)(((unsigned)(3883651629u) * (unsigned)(st7.f0))))))) << ((unsigned)(st6.f0) & 31u))));
      }
      { unsigned sel26 = (unsigned)(((unsigned)(((unsigned)(1059282471u) ^ (unsigned)(1186321439u))) + (unsigned)(u4))) & 63u;
        switch (sel26) {
          cs = csmix(cs, 1962565254u);
          cs = csmix(cs, 3991675283u);
          cs = csmix(cs, (unsigned)(4213326557u));
          cs = csmix(cs, 710937328u);
          cs = csmix(cs, (unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)(u5) & (unsigned)(((unsigned)((~((unsigned)(u5) | 0u))) << ((unsigned)((unsigned)(s2)) & 31u))))))));
          cs = csmix(cs, 1522738314u);
        default: cs = csmix(cs, 17u); break;
        } }
      cs = csmix(cs, 3855421983u);
      cs = csmix(cs, 3659826504u);
      { unsigned g28 = 0u;
        while (g28 < 11u) {
          unsigned i27 = g28;
          cs = csmix(cs, i27);
        }
      }
      { unsigned g30 = 0u;
        while (g30 < 4u) {
          unsigned i29 = g30;
          cs = csmix(cs, i29);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(i29) & 31u))) * (unsigned)(((unsigned)(2767606968u) * (unsigned)(st7.f0))))) & (unsigned)((-((unsigned)(u3) | 0u))))) << ((unsigned)(3823619283u) & 31u))));
        }
      }
      cs = csmix(cs, 3398113197u);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(1616120061u) & 1u) ? (unsigned)(u5) : (unsigned)(1170153210u))) << ((unsigned)(helper1(u5, 1705593502u)) & 31u))) << ((unsigned)(((unsigned)((-((unsigned)(u5) | 0u))) | (unsigned)(1716548237u))) & 31u))) + (unsigned)(st6.f0))));
      cs = csmix(cs, 1144423625u);
      cs = csmix(cs, (unsigned)((-((unsigned)((unsigned)(s2)) | 0u))));
      cs = csmix(cs, 4226446390u);
      cs = csmix(cs, 2625073698u);
      if ((unsigned)(((unsigned)(((unsigned)(367894819u) >> ((unsigned)(381940962u) & 31u))) << ((unsigned)(helper1(((unsigned)(u4) >> ((unsigned)(2289659201u) & 31u)), ((unsigned)((-((unsigned)(u5) | 0u))) & (unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(613073967u) & 31u)))))) & 31u))) & 1u) {
        cs = csmix(cs, (unsigned)(u5));
        cs = csmix(cs, (unsigned)(((unsigned)((~((unsigned)(1515094644u) | 0u))) % ((unsigned)(st6.f2) | 1u))));
      }
      cs = csmix(cs, 69946570u);
      cs = csmix(cs, 1224432797u);
    default: cs = csmix(cs, 89u); break;
    } }
  { unsigned sel31 = (unsigned)(((unsigned)(u3) <= ((unsigned)(st7.f0) ^ cs))) & 3u;
    switch (sel31) {
      cs = csmix(cs, 2790071308u);
      if ((unsigned)(((unsigned)(u3) % ((unsigned)(((unsigned)(((unsigned)(((unsigned)(1068482343u) * (unsigned)(u4))) - (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(1833414882u) & 31u))))) & (unsigned)(1993351642u))) | 1u))) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(((unsigned)(1775673624u) >> ((unsigned)((-((unsigned)((((unsigned)(u5) & 1u) ? (unsigned)(st7.f1) : (unsigned)(((unsigned)(st7.f1) ^ cs)))) | 0u))) & 31u))))));
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) % ((unsigned)(94044271u) | 1u))) / ((unsigned)(3450482361u) | 1u))));
      }
      cs = csmix(cs, 3892527467u);
      if ((unsigned)(st7.f1) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(st7.f0) >> ((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(973329818u) & 31u))) & 31u))) & (unsigned)((unsigned)(s2)))) * (unsigned)(((unsigned)(helper1(u3, ((unsigned)(u4) + (unsigned)(((unsigned)(u4) ^ cs))))) >> ((unsigned)(((unsigned)(((unsigned)(u5) | (unsigned)(st6.f1))) - (unsigned)(((unsigned)(4164598245u) << ((unsigned)(st7.f1) & 31u))))) & 31u))))));
        cs = csmix(cs, (unsigned)(st7.f0));
        cs = csmix(cs, (unsigned)(((unsigned)(2037172773u) != ((unsigned)(((unsigned)(((unsigned)(546316647u) - (unsigned)(((unsigned)(st6.f0) / ((unsigned)(((unsigned)(st6.f0) ^ cs)) | 1u))))) + (unsigned)(((unsigned)(((unsigned)(u5) % ((unsigned)(480924102u) | 1u))) / ((unsigned)(u4) | 1u))))) ^ cs))));
      }
      cs = csmix(cs, 3172468518u);
      cs = csmix(cs, 994125491u);
    default: cs = csmix(cs, 255u); break;
    } }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, st6.f0);
  cs = csmix(cs, st6.f1);
  cs = csmix(cs, st6.f2);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
