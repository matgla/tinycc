/* Fuzz seed struct_byval:6988 (-O1/-O2 wrong code).  A `cond ? (a > b) : call()`
 * diamond assigns the same temp T in both arms (SETIF in the then-arm, a CALL
 * result in the else-arm) and the merge reads it via `U <- T XOR k`.  After k
 * folded to #0, setif_xor_invert saw `U <- T XOR #0` with the previous non-NOP
 * being the then-arm SETIF and hoisted it to the merge as an unconditional
 * SETIF — but the merge is a control-flow join reachable from the else arm
 * (via a jump landing on an intervening NOP), so it dropped the else value and
 * read stale flags.  Fix: setif_xor_invert bails when any instruction between
 * the SETIF and the XOR is a jump target (branch.c).  Correct = gcc = tcc -O0. */
#include <stdio.h>
static unsigned csmix(unsigned h, unsigned v)
{
  h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
  return h * 2654435761u;
}
static unsigned helper1(unsigned pa, unsigned pb)
{
  unsigned lr = pa ^ (pb * 3u);
  return (unsigned)(((unsigned)(((unsigned)(((unsigned)(3596740072u) ^ (unsigned)(3234997900u))) | (unsigned)(pb))) + (unsigned)(((unsigned)(pa) ^ (unsigned)(((unsigned)(lr) % ((unsigned)(pa) | 1u))))))) ^ lr;
}
struct SB1 { unsigned char a; };
struct SB4 { unsigned a; };
struct SB5 { unsigned a; unsigned char b; };
union UB { unsigned w; unsigned char b; };
static struct SB5 sbh2(struct SB1 p, unsigned x)
{
  struct SB5 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu, (unsigned)(((unsigned)(x) >= ((unsigned)(((unsigned)(((unsigned)(2603063088u) + (unsigned)(1782559058u))) / ((unsigned)(((unsigned)(3571795358u) % ((unsigned)(1429770109u) | 1u))) | 1u))) ^ x))) & 0xffu };
  return r;
}
static struct SB4 sbh3(struct SB4 p, unsigned x)
{
  struct SB4 r = { (unsigned)(x ^ (p.a * 3u)) & 0xffffffffu };
  return r;
}
struct S {
};
int main(void)
{
  unsigned cs = 0x12345678u;
  int s4 = (int)(1071954984u & 0xffffffff);
  long s5 = (long)(1889941625u & 0xffffffff);
  unsigned u6 = 2453931920u;
  unsigned u7 = 4204276674u;
  unsigned u8 = 3029430689u;
  unsigned u9 = 1015926555u;
  cs = csmix(cs, (unsigned)((~((unsigned)(helper1((-((unsigned)(u8) | 0u)), ((unsigned)(u6) | (unsigned)(((unsigned)((unsigned)(s4)) - (unsigned)(((unsigned)((unsigned)(s4)) ^ cs))))))) | 0u))));
  { union UB ub10; ub10.w = (unsigned)(((unsigned)(((unsigned)((~((unsigned)((unsigned)(s5)) | 0u))) * (unsigned)(u8))) / ((unsigned)(u9) | 1u))); cs = csmix(cs, ub10.w); }
  cs = csmix(cs, (unsigned)(((unsigned)(u8) + (unsigned)(((unsigned)(u9) + (unsigned)(((unsigned)((((unsigned)((unsigned)(s5)) & 1u) ? (unsigned)(u8) : (unsigned)((unsigned)(s4)))) & (unsigned)(((unsigned)(835350828u) - (unsigned)(3254664350u))))))))));
  { unsigned g12 = 0u;
    while (g12 < 5u) {
      unsigned i11 = g12;
      cs = csmix(cs, i11);
      cs = csmix(cs, (unsigned)(3240781195u));
      cs = csmix(cs, (unsigned)(2931543150u));
      { struct SB4 sba13 = { (unsigned)(helper1(1362724586u, 1479204257u)) & 0xffffffffu };
        struct SB4 sbt14 = sbh3(sba13, (unsigned)(u7));
        cs = csmix(cs, sbt14.a);
      }
      { struct SB1 sba15 = { (unsigned)(((unsigned)(1803811156u) * (unsigned)(((unsigned)(u8) % ((unsigned)(3961526945u) | 1u))))) & 0xffu };
        struct SB5 sbt16 = sbh2(sba15, (unsigned)(((unsigned)(((unsigned)(((unsigned)(((unsigned)(2324038248u) >> ((unsigned)(2198967724u) & 31u))) >> ((unsigned)(i11) & 31u))) * (unsigned)(((unsigned)(((unsigned)(u9) & (unsigned)(4100703460u))) | (unsigned)(((unsigned)(i11) ^ (unsigned)(3083774258u))))))) + (unsigned)(1063689322u))));
        cs = csmix(cs, sbt16.a);
        cs = csmix(cs, sbt16.b);
      }
      for (unsigned g18 = 0u; g18 < 9u; g18++) {
        unsigned i17 = g18;
        cs = csmix(cs, i17);
        cs = csmix(cs, (unsigned)(((unsigned)(((unsigned)((((unsigned)(u7) & 1u) ? (unsigned)((unsigned)(s4)) : (unsigned)(((unsigned)(911242726u) * (unsigned)(u6))))) ^ (unsigned)(i17))) & (unsigned)(((unsigned)((((unsigned)(((unsigned)(i11) + (unsigned)(u8))) & 1u) ? (unsigned)(((unsigned)(u7) > ((unsigned)(((unsigned)(u7) ^ cs)) ^ cs))) : (unsigned)(helper1((unsigned)(s5), 1546486853u)))) ^ (unsigned)(((unsigned)(3252101413u) & (unsigned)(((unsigned)(u8) / ((unsigned)(34345126u) | 1u))))))))));
        cs = csmix(cs, (unsigned)(((unsigned)(u7) ^ (unsigned)(u6))));
        { struct SB1 sba19 = { (unsigned)(i17) & 0xffu };
          struct SB5 sbt20 = sbh2(sba19, (unsigned)(((unsigned)((~((unsigned)(helper1(((unsigned)(u7) >> ((unsigned)(3180307520u) & 31u)), ((unsigned)(u8) - (unsigned)(3817000480u)))) | 0u))) << ((unsigned)(((unsigned)(((unsigned)(((unsigned)(3633797084u) < ((unsigned)(u8) ^ cs))) + (unsigned)(helper1(105055912u, 3999147884u)))) / ((unsigned)(2359230609u) | 1u))) & 31u))));
          cs = csmix(cs, sbt20.a);
          cs = csmix(cs, sbt20.b);
        }
      }
      g12++;
    }
  }
  { struct SB1 sba21 = { (unsigned)((-((unsigned)(((unsigned)(1929695624u) >> ((unsigned)(2242758753u) & 31u))) | 0u))) & 0xffu };
    struct SB5 sbt22 = sbh2(sba21, (unsigned)(((unsigned)((((unsigned)(2862259498u) & 1u) ? (unsigned)(278151530u) : (unsigned)(((unsigned)(((unsigned)(4190572077u) * (unsigned)((unsigned)(s4)))) * (unsigned)((unsigned)(s5)))))) + (unsigned)(((unsigned)(u6) > ((unsigned)(((unsigned)(u6) ^ cs)) ^ cs))))));
    cs = csmix(cs, sbt22.a);
    cs = csmix(cs, sbt22.b);
  }
  cs = csmix(cs, u6);
  cs = csmix(cs, u7);
  cs = csmix(cs, u8);
  cs = csmix(cs, u9);
  cs = csmix(cs, helper1(1u, cs));
  cs = csmix(cs, (unsigned)s4);
  cs = csmix(cs, (unsigned)s5);
  { struct SB1 sba23 = { 1u };
    struct SB5 sbt24 = sbh2(sba23, cs);
    cs = csmix(cs, sbt24.a);
    cs = csmix(cs, sbt24.b); }
  { struct SB4 sba25 = { 19088744u };
    struct SB4 sbt26 = sbh3(sba25, cs);
    cs = csmix(cs, sbt26.a); }
  printf("checksum=%08x\n", cs);
  return 0;
}
