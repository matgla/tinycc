/* switch fuzz seed 17829: known_bits reused stack-slot facts from one switch
 * case on a direct jump to a later fall-through case because SWITCH_TABLE
 * targets were not marked as block starts. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s1 = (int)(1747767481u & 0xffffffff);
  short s2 = (short)(1064290229u & 0xffff);
  unsigned u3 = 1264824242u;
  unsigned u4 = 3188514616u;
  unsigned u5 = 3762569784u;
  unsigned u6 = 2349050902u;
  unsigned u7 = 2343669870u;
  unsigned arr8[8] = { 3779860584u, 3440818583u, 3640647734u, 4113662859u, 2916596879u, 1104142959u, 4192380854u, 1789097812u };
  struct S st9 = { 754742086u, 1642731645u, 1588859347u };
  struct S st10 = { 370206491u, 335588037u, 664433055u };
  for (unsigned g12 = 0u; g12 < 7u; g12++) {
    unsigned i11 = g12;
    cs = csmix(cs, i11);
    for (unsigned g14 = 0u; g14 < 3u; g14++) {
      unsigned i13 = g14;
      cs = csmix(cs, i13);
      cs = csmix(cs, (unsigned)((unsigned)(s2)));
    }
    { unsigned sel15 = (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(((unsigned)(u7) * (unsigned)((unsigned)(s2)))) & 31u))) & 7u;
      switch (sel15) {
      case 0:
        cs = csmix(cs, (unsigned)(arr8[((unsigned)(u5) & 7u)]));
        u4 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(i11) & 1u) ? (unsigned)((unsigned)(s1)) : (unsigned)((unsigned)(s2)))) & (unsigned)(((unsigned)(u7) << ((unsigned)(st10.f1) & 31u))))) & (unsigned)(2106918525u))) >> ((unsigned)(4042342587u) & 31u))) & 0xffffffffu;
        cs = csmix(cs, 902662736u);
        cs = csmix(cs, 1354920147u);
        break;
      case 2:
        st9.f0 = (unsigned)(u3);
        cs = csmix(cs, 2113131594u);
        cs = csmix(cs, 3577270726u);
      case 4:
        arr8[((unsigned)(u6) & 7u)] = (unsigned)(st9.f0);
        cs = csmix(cs, 2444036713u);
        cs = csmix(cs, 3599304713u);
        cs = csmix(cs, 3697057123u);
      case 7:
        cs = csmix(cs, 2137200714u);
      default: cs = csmix(cs, 66u); break;
      } }
    u7 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) + (unsigned)(((unsigned)(u4) >= ((unsigned)(arr8[((unsigned)(u4) & 7u)]) ^ cs))))) - (unsigned)((-((unsigned)(29160651u) | 0u))))) | (unsigned)(((unsigned)(u4) >> ((unsigned)(((unsigned)(u6) - (unsigned)(((unsigned)(u7) ^ (unsigned)(st9.f2))))) & 31u))))) & 0xffffffffu;
    { unsigned sel16 = (unsigned)(1414705962u) & 63u;
      switch (sel16) {
        cs = csmix(cs, 367793124u);
        cs = csmix(cs, (unsigned)(arr8[((unsigned)(u4) & 7u)]));
        cs = csmix(cs, 2385662727u);
        cs = csmix(cs, 3327582706u);
        cs = csmix(cs, 1471395917u);
        cs = csmix(cs, 1855745755u);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(i11) / ((unsigned)(((unsigned)(i11) ^ cs)) | 1u))) % ((unsigned)(arr8[((unsigned)(i11) & 7u)]) | 1u))) / ((unsigned)((unsigned)(s2)) | 1u))) * (unsigned)(arr8[((unsigned)(796457674u) & 7u)]))));
        cs = csmix(cs, (unsigned)(u5));
        cs = csmix(cs, 2097541053u);
        cs = csmix(cs, 1690914693u);
        cs = csmix(cs, 294450215u);
        cs = csmix(cs, 3217422606u);
      default: cs = csmix(cs, 162u); break;
      } }
    cs = csmix(cs, (unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) % ((unsigned)((~((unsigned)(((unsigned)(((unsigned)(u5) << ((unsigned)((unsigned)(s1)) & 31u))) + (unsigned)(((unsigned)(u7) << ((unsigned)((unsigned)(s1)) & 31u))))) | 0u))) | 1u))));
  }
  if ((unsigned)(2594614903u) & 1u) {
    cs = csmix(cs, (unsigned)(2944959082u));
    { unsigned sel17 = (unsigned)(((unsigned)(((unsigned)((unsigned)(s1)) - (unsigned)((-((unsigned)(((unsigned)(u4) << ((unsigned)(arr8[((unsigned)(u7) & 7u)]) & 31u))) | 0u))))) % ((unsigned)(st9.f2) | 1u))) & 63u;
      switch (sel17) {
        cs = csmix(cs, (unsigned)(((unsigned)(arr8[((unsigned)(1363721446u) & 7u)]) | (unsigned)(1747746337u))));
        cs = csmix(cs, 616276280u);
        cs = csmix(cs, (unsigned)(((unsigned)(u4) << ((unsigned)(arr8[((unsigned)(855661753u) & 7u)]) & 31u))));
        cs = csmix(cs, 774916979u);
        cs = csmix(cs, (unsigned)(st10.f1));
        cs = csmix(cs, 1522380277u);
        cs = csmix(cs, 3631943224u);
        cs = csmix(cs, (unsigned)(2067529399u));
        cs = csmix(cs, 3465469458u);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) | (unsigned)(1351212033u))) - (unsigned)(u4))) >> ((unsigned)(u5) & 31u))) >> ((unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) >= ((unsigned)(2631544925u) ^ cs))) == ((unsigned)(((unsigned)(2033589380u) / ((unsigned)(4191719557u) | 1u))) ^ cs))) << ((unsigned)((((unsigned)(arr8[((unsigned)(u5) & 7u)]) & 1u) ? (unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) >> ((unsigned)(4003373706u) & 31u))) : (unsigned)(((unsigned)(arr8[((unsigned)(232807420u) & 7u)]) >> ((unsigned)(u5) & 31u))))) & 31u))) & 31u))));
        cs = csmix(cs, 3367138201u);
      default: cs = csmix(cs, 151u); break;
      } }
  }
  cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s1)) <= ((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(st10.f1) & 31u))) / ((unsigned)(((unsigned)((unsigned)(s2)) & (unsigned)(u4))) | 1u))) | (unsigned)(((unsigned)(((unsigned)(968836094u) << ((unsigned)(3237809639u) & 31u))) | (unsigned)(((unsigned)(st10.f2) * (unsigned)(3972040220u))))))) ^ cs))));
  cs = csmix(cs, (unsigned)(1878570920u));
  for (unsigned g19 = 0u; g19 < 7u; g19++) {
    unsigned i18 = g19;
    cs = csmix(cs, i18);
    { unsigned sel20 = (unsigned)(((unsigned)(((unsigned)(((unsigned)((((unsigned)(1285854698u) & 1u) ? (unsigned)(u4) : (unsigned)(arr8[((unsigned)(u4) & 7u)]))) & (unsigned)(((unsigned)(arr8[((unsigned)(u3) & 7u)]) + (unsigned)(u3))))) + (unsigned)(((unsigned)(((unsigned)(arr8[((unsigned)(u7) & 7u)]) + (unsigned)(i18))) < ((unsigned)(960473917u) ^ cs))))) & (unsigned)(u7))) & 7u;
      switch (sel20) {
        cs = csmix(cs, 800951329u);
        cs = csmix(cs, 770836669u);
        cs = csmix(cs, 1266783745u);
        cs = csmix(cs, (unsigned)((-((unsigned)(((unsigned)((-((unsigned)(4250229519u) | 0u))) / ((unsigned)(u3) | 1u))) | 0u))));
        cs = csmix(cs, 2223032006u);
        cs = csmix(cs, 3981214028u);
        cs = csmix(cs, (unsigned)(((unsigned)(u6) & (unsigned)(2501222374u))));
        cs = csmix(cs, 575732094u);
        cs = csmix(cs, 69888291u);
      default: cs = csmix(cs, 26u); break;
      } }
    cs = csmix(cs, (unsigned)(((unsigned)(i18) << ((unsigned)(((unsigned)(u3) & (unsigned)(((unsigned)(((unsigned)(u5) | (unsigned)(1515060734u))) + (unsigned)(2882385837u))))) & 31u))));
    if ((unsigned)((-((unsigned)(3163369060u) | 0u))) & 1u) {
      cs = csmix(cs, (unsigned)(3969946046u));
    }
  }
  for (unsigned g22 = 0u; g22 < 9u; g22++) {
    unsigned i21 = g22;
    cs = csmix(cs, i21);
    if ((unsigned)(((unsigned)(arr8[((unsigned)(i21) & 7u)]) + (unsigned)(u7))) & 1u) {
      cs = csmix(cs, (unsigned)(((unsigned)((-((unsigned)(((unsigned)(u7) % ((unsigned)(((unsigned)(st10.f0) <= ((unsigned)((unsigned)(s2)) ^ cs))) | 1u))) | 0u))) - (unsigned)(((unsigned)(((unsigned)(((unsigned)(u7) * (unsigned)(3482858065u))) ^ (unsigned)(((unsigned)(st10.f0) - (unsigned)(u7))))) % ((unsigned)(u3) | 1u))))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(u5) << ((unsigned)(u6) & 31u))) / ((unsigned)(((unsigned)(u4) ^ (unsigned)(((unsigned)(u4) ^ cs)))) | 1u))) << ((unsigned)(((unsigned)(((unsigned)(3807943947u) / ((unsigned)(1595597331u) | 1u))) + (unsigned)(((unsigned)(arr8[((unsigned)(u5) & 7u)]) != ((unsigned)((unsigned)(s2)) ^ cs))))) & 31u))) * (unsigned)(((unsigned)(arr8[((unsigned)(3794985144u) & 7u)]) / ((unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(st9.f1) & 31u))) | 1u))))));
    }
    { unsigned g24 = 0u;
      while (g24 < 7u) {
        unsigned i23 = g24;
        cs = csmix(cs, i23);
        g24++;
      }
    }
  }
  cs = csmix(cs, u3);
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, (unsigned)s1);
  cs = csmix(cs, (unsigned)s2);
  for (unsigned k = 0u; k < 8u; k++) cs = csmix(cs, arr8[k]);
  cs = csmix(cs, st9.f0);
  cs = csmix(cs, st9.f1);
  cs = csmix(cs, st9.f2);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
