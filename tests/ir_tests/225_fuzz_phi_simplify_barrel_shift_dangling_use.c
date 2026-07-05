/* Regression test (reduced differential-fuzz repro, seed 19826).
 *
 * A loop-invariant local `u4` is assigned `st7.f2` inside the first loop and
 * read again after the loops.  Out-of-SSA gave it a loop-closing phi; that phi
 * was trivial (one real operand), so ssa:phi_simplify tried to fold it away by
 * replacing every use of the phi-dest with the operand.  But one post-loop use
 * is an ARM barrel-shift source (`u4 >> n`): ssa_opt_replace_all_uses refuses
 * to rewrite a barrel-shift src2 (the implicit shift is keyed on the vreg) and
 * bails, rewriting NOTHING — yet phi_simplify removed the phi anyway.  The def
 * vanished while the uses stayed, leaving `u4` reading an undefined (zero)
 * stack slot, so the tail `csmix(cs, u4)` mixed 0 instead of 0xbe954e5c.
 *
 * tcc -O0/-O1 were correct; only -O2 (where u4 is promoted + spilled under
 * register pressure, so the phi exists) miscompiled.  Fix: ir/opt/ssa_opt_phi.c
 * keeps the phi when its dest still has uses after the replacement attempt.
 * Expected checksum is gcc -m32 -funsigned-char.
 */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  h = (h << 13) | (h >> 19);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(lr) | (unsigned)(pa))) * (unsigned)(((unsigned)(pb) & (unsigned)(lr))))) + (unsigned)(((unsigned)(lr) & (unsigned)((-((unsigned)(pb) | 0u))))))) ^ lr;
}
struct S {
  unsigned f0;
  unsigned f1;
  unsigned f2;
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s2 = (int)(918621027u & 0xffffffff);
  int s3 = (int)(2039430197u & 0xffffffff);
  unsigned u4 = 3169192790u;
  unsigned u5 = 1796678336u;
  struct S st6 = { 3763690037u, 812873685u, 3743956285u };
  struct S st7 = { 3394330352u, 1202665537u, 3197455964u };
  cs = csmix(cs, (unsigned)(((unsigned)(helper1((unsigned)(s2), 2688933947u)) * (unsigned)(((unsigned)(((unsigned)(u5) + (unsigned)(((unsigned)(2529349591u) | (unsigned)(u4))))) << ((unsigned)(u4) & 31u))))));
  for (unsigned g9 = 0u; g9 < 9u; g9++) {
    unsigned i8 = g9;
    cs = csmix(cs, i8);
    cs = csmix(cs, (unsigned)(1156122824u));
    u4 = (unsigned)(st7.f2) & 0xffffffffu;
    cs = csmix(cs, (unsigned)(((unsigned)(u4) & (unsigned)(((unsigned)(st6.f2) == ((unsigned)(1004129606u) ^ cs))))));
    cs = csmix(cs, (unsigned)((unsigned)(s3)));
    for (unsigned g11 = 0u; g11 < 2u; g11++) {
      unsigned i10 = g11;
      cs = csmix(cs, i10);
      u5 = (unsigned)((((unsigned)(((unsigned)(st7.f2) << ((unsigned)(((unsigned)(u5) / ((unsigned)((((unsigned)(i10) & 1u) ? (unsigned)((unsigned)(s2)) : (unsigned)(2797263902u))) | 1u))) & 31u))) & 1u) ? (unsigned)(((unsigned)(((unsigned)(3444147692u) + (unsigned)(((unsigned)(i8) | (unsigned)(u4))))) - (unsigned)(((unsigned)(((unsigned)(st6.f0) % ((unsigned)((unsigned)(s2)) | 1u))) >= ((unsigned)(((unsigned)(1436762444u) << ((unsigned)((unsigned)(s2)) & 31u))) ^ cs))))) : (unsigned)(131389288u))) & 0xffffffffu;
      cs = csmix(cs, (unsigned)(st7.f1));
      u5 = (unsigned)((((unsigned)(3175394515u) & 1u) ? (unsigned)(((unsigned)(((unsigned)(((unsigned)(st7.f1) & (unsigned)(3338754371u))) << ((unsigned)(((unsigned)(i10) << ((unsigned)(i8) & 31u))) & 31u))) + (unsigned)((unsigned)(s3)))) : (unsigned)((~((unsigned)(u4) | 0u))))) & 0xffffffffu;
    }
  }
  { unsigned g13 = 0u;
    while (g13 < 8u) {
      unsigned i12 = g13;
      cs = csmix(cs, i12);
      st6.f0 = (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(i12) | (unsigned)(3529159395u))) * (unsigned)(2198207610u))) * (unsigned)(((unsigned)(((unsigned)(u4) >> ((unsigned)((unsigned)(s3)) & 31u))) + (unsigned)(((unsigned)(st6.f2) - (unsigned)(1261256402u))))))) + (unsigned)(u5)));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(3610092572u) > ((unsigned)(3998304300u) ^ cs))) & (unsigned)(u5))) | (unsigned)(((unsigned)(u4) >> ((unsigned)(i12) & 31u))))));
      g13++;
    }
  }
  for (unsigned g15 = 0u; g15 < 12u; g15++) {
    unsigned i14 = g15;
    cs = csmix(cs, i14);
    { unsigned g17 = 0u;
      while (g17 < 10u) {
        unsigned i16 = g17;
        cs = csmix(cs, i16);
        cs = csmix(cs, (unsigned)(((unsigned)((unsigned)(s2)) << ((unsigned)(((unsigned)(i16) << ((unsigned)(((unsigned)(44895377u) - (unsigned)(i14))) & 31u))) & 31u))));
        g17++;
      }
    }
  }
  for (unsigned g19 = 0u; g19 < 8u; g19++) {
    unsigned i18 = g19;
    cs = csmix(cs, i18);
    for (unsigned g21 = 0u; g21 < 7u; g21++) {
      unsigned i20 = g21;
      cs = csmix(cs, i20);
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(st6.f2) ^ (unsigned)(((unsigned)(((unsigned)(i20) % ((unsigned)((unsigned)(s2)) | 1u))) + (unsigned)(((unsigned)((unsigned)(s3)) - (unsigned)(i20))))))) ^ (unsigned)(4210857952u))));
      cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)((unsigned)(s2)) >> ((unsigned)(2569926999u) & 31u))) + (unsigned)(st6.f1))) ^ (unsigned)(55429540u))) / ((unsigned)(st7.f0) | 1u))));
    }
  }
  cs = csmix(cs, u4);
  cs = csmix(cs, u5);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s2);
  cs = csmix(cs, (unsigned)s3);
  cs = csmix(cs, st6.f0);
  cs = csmix(cs, st6.f1);
  cs = csmix(cs, st6.f2);
  cs = csmix(cs, st7.f0);
  cs = csmix(cs, st7.f1);
  cs = csmix(cs, st7.f2);
  printf("checksum=%08x\n", cs);
  return 0;
}
