/* switch fuzz seed 18613: full unroll grew case 0's counted loop without
 * shifting later SWITCH_TABLE case/default targets.  Selector 3 then entered
 * the wrong point in the fall-through case chain. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)((~((unsigned)(pb) | 0u))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(1726595051u & 0xffffffff);
  char s3 = (char)(1310649575u & 0xff);
  unsigned u4 = 3989692654u;
  unsigned u5 = 3094626681u;
  unsigned u6 = 2553578490u;
  unsigned u7 = 2343123u;
  unsigned u8 = 2035398391u;
  unsigned u9 = 30702347u;
  struct S st10 = { 1838652102u, 2611614913u, 3630269837u };
  cs = csmix(cs, (unsigned)(u8));
  { unsigned sel11 = (unsigned)(((unsigned)((((unsigned)((-((unsigned)(helper1(u9, (unsigned)(s2))) | 0u))) & 1u) ? (unsigned)((~((unsigned)(867846380u) | 0u))) : (unsigned)(st10.f0))) >> ((unsigned)(883260811u) & 31u))) & 7u;
    switch (sel11) {
    case 0:
      { unsigned g13 = 0u;
        while (g13 < 3u) {
          unsigned i12 = g13;
          cs = csmix(cs, i12);
          cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(i12) & (unsigned)(((unsigned)((-((unsigned)(st10.f2) | 0u))) >> ((unsigned)(u6) & 31u))))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(2897862044u) - (unsigned)(u7))) / ((unsigned)(((unsigned)(3677839272u) * (unsigned)((unsigned)(s2)))) | 1u))) | (unsigned)(3819949009u))) & 31u))));
          g13++;
        }
      }
      cs = csmix(cs, 3530207549u);
      if ((unsigned)((unsigned)(s2)) & 1u) {
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(2657065466u) / ((unsigned)(u6) | 1u))) + (unsigned)((~((unsigned)(((unsigned)(((unsigned)(1110864463u) - (unsigned)(u9))) / ((unsigned)((-((unsigned)(u6) | 0u))) | 1u))) | 0u))))));
        cs = csmix(cs, (unsigned)((((unsigned)(st10.f2) & 1u) ? (unsigned)(((unsigned)(((unsigned)(helper1(u8, u4)) >= ((unsigned)(((unsigned)(st10.f2) > ((unsigned)((unsigned)(s2)) ^ cs))) ^ cs))) & (unsigned)(st10.f0))) : (unsigned)(((unsigned)(((unsigned)(u4) << ((unsigned)(u7) & 31u))) ^ (unsigned)(((unsigned)(helper1(1403141264u, 2242476242u)) | (unsigned)((((unsigned)(3494533661u) & 1u) ? (unsigned)(u9) : (unsigned)(u5))))))))));
      }
      cs = csmix(cs, 3396072579u);
      cs = csmix(cs, 1174972510u);
      cs = csmix(cs, 4208694089u);
    case 4:
      cs = csmix(cs, 1404935606u);
    case 5:
      cs = csmix(cs, 3290286062u);
    case 6:
      cs = csmix(cs, 3598653222u);
    default: cs = csmix(cs, 225u); break;
    } }
  cs = csmix(cs, (unsigned)(2289483897u));
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st10.f0);
  cs = csmix(cs, st10.f1);
  cs = csmix(cs, st10.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
